#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <sys/mman.h>
#include <unistd.h>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

struct RingDeleter {
    size_t bytes{0};
    bool used_mmap{false};

    void operator()(void* p) const noexcept {
        if (!p) {
            return;
        }

        if (used_mmap) {
            ::munmap(p, bytes);
        } else {
            ::free(p);
        }
    }
};

struct RingAllocOpt {
    size_t align = 64;
    bool try_huge = true;
    bool prefault = true;
    bool mlock_pages = true;
    int numa_node = -1;
};

inline std::unique_ptr<void, RingDeleter>
allocate_ring_bytes(size_t nbytes, const RingAllocOpt& opt, bool& got_huge) {
    got_huge = false;

    const size_t HUGEPG = 2 * 1024 * 1024;
    const bool large = nbytes >= HUGEPG;

    if (opt.try_huge) {
        size_t map_len = (nbytes + HUGEPG - 1) / HUGEPG * HUGEPG;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        void* p = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, flags, -1, 0);

        if (p != MAP_FAILED && p) {
            (void)::madvise(p, map_len, MADV_HUGEPAGE);
            if (opt.prefault) {
                (void)::madvise(p, map_len, MADV_WILLNEED);
            }
            if (opt.mlock_pages) {
                (void)::mlock(p, map_len);
            }
            got_huge = true;
            return {p, RingDeleter{map_len, true}};
        }
    }

    // void* p = nullptr;
    // size_t align = opt.align;
    // if (align < alignof(std::max_align_t)) {
    //     align = alignof(std::max_align_t);
    // }
    // if (int rc = ::posix_memalign(&p, align, nbytes); rc != 0 || !p) {
    //     throw std::bad_alloc();
    // }
    // if (opt.prefault) {
    //     std::memset(p, 0, nbytes);
    // }
    // return {p, RingDeleter{nbytes, false}};
}

template <typename T, size_t SIZE>
class LockFreeQueue {
    static_assert((SIZE & (SIZE - 1)) == 0, "size must be power of 2");
    static constexpr size_t CAPACITY = SIZE;
    static constexpr size_t MASK = CAPACITY - 1;
    static constexpr size_t PADDING = (CACHE_LINE_SIZE - 1) / sizeof(T) + 1;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    alignas(CACHE_LINE_SIZE) size_t head_cache_{0};
    alignas(CACHE_LINE_SIZE) size_t tail_cache_{0};

    using Slot = std::aligned_storage_t<sizeof(T), alignof(T)>;
    const size_t nslots_ = CAPACITY + 2 * PADDING;
    Slot* base_{nullptr};
    bool using_huge_{false};
    std::unique_ptr<void, RingDeleter> mem_;

    T* get_slot(size_t idx) noexcept {
        return reinterpret_cast<T*>(base_ + (idx & MASK) + PADDING);
    }

public:
    explicit LockFreeQueue(const RingAllocOpt& opt = {}) {
        const size_t bytes = nslots_ * sizeof(Slot);
        mem_ = allocate_ring_bytes(bytes, opt, using_huge_);
        base_ = reinterpret_cast<Slot*>(mem_.get());
        if (!base_) {
            throw std::bad_alloc();
        }
    }

    ~LockFreeQueue() {
        size_t curr_head = head_.load(std::memory_order_acquire);
        size_t curr_tail = tail_.load(std::memory_order_acquire);
        while (curr_head != curr_tail) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                get_slot(curr_head)->~T();
            }
            curr_head = (curr_head + 1) & MASK;
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    template <typename U>
    bool enqueue(U&& item) {
        const size_t curr_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (curr_tail + 1) & MASK;

        if (next_tail == head_cache_) [[unlikely]] {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next_tail == head_cache_) return false;
        }

        //__builtin_prefetch(get_slot(next_tail), 1, 1);
        new (get_slot(curr_tail)) T(std::forward<U>(item));
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    T* front() {
        const size_t curr_head = head_.load(std::memory_order_relaxed);
        if (curr_head == tail_cache_) [[unlikely]] {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (curr_head == tail_cache_) {
                return nullptr;
            }
        }

        //__builtin_prefetch(get_slot((curr_head + 1) & MASK), 0, 1);
        return get_slot(curr_head);
    }

    void pop() {
        const size_t curr_head = head_.load(std::memory_order_relaxed);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            get_slot(curr_head)->~T();
        }

        head_.store((curr_head + 1) & MASK, std::memory_order_release);
    }

    std::optional<T> dequeue() {
        const size_t curr_head = head_.load(std::memory_order_relaxed);
        if (curr_head == tail_cache_) [[unlikely]] {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (curr_head == tail_cache_) return std::nullopt;
        }

        T* item_ptr = get_slot(curr_head);
        std::optional<T> result(std::move(*item_ptr));
        if constexpr (!std::is_trivially_destructible_v<T>) {
            item_ptr->~T();
        }

        head_.store((curr_head + 1) & MASK, std::memory_order_release);
        return result;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (t - h) & MASK;
    }

    size_t capacity() const { return CAPACITY - 1; }

    bool using_huge_pages() const noexcept { return using_huge_; }
};
