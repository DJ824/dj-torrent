#pragma once

#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <vector>

class BlockBitmap {
public:
    explicit BlockBitmap(std::size_t blocks)
        : bits_((blocks + 7) / 8, 0), total_(blocks), have_(0) {
    }

    void set(std::size_t idx) {
        if (idx >= total_) {
            throw std::out_of_range("Block index out of range");
        }
        std::size_t byte = idx / 8;
        uint8_t mask = static_cast<uint8_t>(1u << (7 - (idx % 8)));
        if ((bits_[byte] & mask) == 0) {
            bits_[byte] |= mask;
            ++have_;
        }
    }

    bool test(std::size_t idx) const {
        if (idx >= total_) {
            throw std::out_of_range("Block index out of range");
        }
        std::size_t byte = idx / 8;
        uint8_t mask = static_cast<uint8_t>(1u << (7 - (idx % 8)));
        return (bits_[byte] & mask) != 0;
    }

    std::size_t count() const { return have_; }
    std::size_t total() const { return total_; }
    bool full() const { return have_ == total_; }

private:
    std::vector<uint8_t> bits_;
    std::size_t total_;
    std::size_t have_;
};

class PieceBuffer {
public:
    PieceBuffer(std::size_t piece_index, std::size_t piece_length, std::size_t block_size)
        : index_(piece_index),
          piece_length_(piece_length),
          block_size_(block_size),
          data_(piece_length),
          blocks_((piece_length + block_size - 1) / block_size),
          bitmap_(blocks_) {
    }

    struct BlockWriteResult {
        bool accepted;
        bool complete_now;
    };

    BlockWriteResult write_block(std::size_t offset, const uint8_t* src, std::size_t len) {
        if (offset + len > piece_length_) {
            return {false, false};
        }
        if (len == 0) {
            return {false, false};
        }
        std::size_t block_idx = offset / block_size_;
        std::size_t expected_len = (block_idx == blocks_ - 1)
                                       ? piece_length_ - block_idx * block_size_
                                       : block_size_;
        if (len != expected_len && offset + len != piece_length_) {
            return {false, false};
        }
        if (bitmap_.test(block_idx)) {
            return {false, false};
        }

        std::copy(src, src + len, data_.begin() + static_cast<std::ptrdiff_t>(offset));
        bitmap_.set(block_idx);
        bool completed = bitmap_.full();
        return {true, completed};
    }

    bool complete() const { return bitmap_.full(); }
    const std::vector<uint8_t>& data() const { return data_; }
    std::size_t piece_index() const { return index_; }
    std::size_t piece_length() const { return piece_length_; }

private:
    std::size_t index_;
    std::size_t piece_length_;
    std::size_t block_size_;
    std::vector<uint8_t> data_;
    std::size_t blocks_;
    BlockBitmap bitmap_;
};
