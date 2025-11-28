#include "peer_event_loop.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <array>

PeerEventLoop::PeerEventLoop(EventCallback cb) : callback_(std::move(cb)) {
    epfd_ = epoll_create1(0);
}

PeerEventLoop::~PeerEventLoop() {
    if (epfd_ >= 0) {
        close(epfd_);
    }
}

bool PeerEventLoop::add_peer(Peer peer) {
    if (epfd_ < 0) {
        return false;
    }

    int fd = peer.fd();
    uint32_t events = EPOLLIN | EPOLLOUT;
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return false;
    }

    peers_.emplace(fd, Entry{std::move(peer), events});
    return true;
}

void PeerEventLoop::remove_peer(int fd) {
    if (epfd_ >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    peers_.erase(fd);
}

void PeerEventLoop::run(int timeout_ms) {
    running_ = true;
    while (running_ && !peers_.empty()) {
        run_once(timeout_ms);
    }
}

void PeerEventLoop::stop() { running_ = false; }

Peer* PeerEventLoop::peer_by_fd(int fd) {
    auto it = peers_.find(fd);
    if (it == peers_.end()) {
        return nullptr;
    }
    return &it->second.peer;
}

void PeerEventLoop::for_each_peer(const std::function<void(Peer&)>& fn) {
    for (auto& kv : peers_) {
        fn(kv.second.peer);
    }
}

void PeerEventLoop::run_once(int timeout_ms) {
    if (epfd_ < 0) {
        return;
    }

    std::array<epoll_event, 64> events{};
    int n = epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), timeout_ms);

    if (n <= 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        auto it = peers_.find(fd);
        if (it == peers_.end()) {
            continue;
        }
        Entry& entry = it->second;
        Peer& p = entry.peer;

        uint32_t ev = events[i].events;
        if (ev & (EPOLLERR | EPOLLHUP)) {
            p.handle_error();
        } else {
            if (ev & EPOLLIN) {
                p.handle_readable();
            }
            if ((ev & EPOLLOUT) && !p.is_closed()) {
                p.handle_writable();
            }
        }

        if (callback_) {
            auto parsed = p.drain_events();
            if (!parsed.empty()) {
                callback_(p, std::move(parsed));
            }
        }

        if (p.is_closed()) {
            remove_peer(fd);
            continue;
        }
        update_interest(fd, entry);
    }
}

void PeerEventLoop::update_interest(int fd, Entry& entry) {
    uint32_t want = EPOLLIN;
    if (entry.peer.wants_write()) {
        want |= EPOLLOUT;
    }
    if (want == entry.events) {
        return;
    }
    epoll_event ev{};
    ev.events = want;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
        entry.events = want;
    }
}
