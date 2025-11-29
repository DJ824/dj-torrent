#include "peer_event_loop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <array>
#include <cerrno>

PeerEventLoop::PeerEventLoop(EventCallback cb) : callback_(std::move(cb)) {
    epfd_ = epoll_create1(0);
}

PeerEventLoop::~PeerEventLoop() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
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

bool PeerEventLoop::set_listen_socket(int fd, AcceptCallback cb) {
    if (epfd_ < 0) {
        return false;
    }
    listen_fd_ = fd;
    accept_callback_ = std::move(cb);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        listen_fd_ = -1;
        accept_callback_ = nullptr;
        return false;
    }
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
    while (running_ && (listen_fd_ >= 0 || !peers_.empty())) {
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
        if (fd == listen_fd_) {
            if ((events[i].events & EPOLLIN) && accept_callback_) {
                for (;;) {
                    sockaddr_storage ss{};
                    socklen_t slen = sizeof(ss);
                    int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &slen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }
                    int flags = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

                    PeerAddress addr{};
                    if (ss.ss_family == AF_INET) {
                        auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
                        char ipbuf[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
                        addr.ip = ipbuf;
                        addr.port = ntohs(sin->sin_port);
                    } else if (ss.ss_family == AF_INET6) {
                        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&ss);
                        char ipbuf[INET6_ADDRSTRLEN]{};
                        inet_ntop(AF_INET6, &sin6->sin6_addr, ipbuf, sizeof(ipbuf));
                        addr.ip = ipbuf;
                        addr.port = ntohs(sin6->sin6_port);
                    } else {
                        ::close(cfd);
                        continue;
                    }

                    accept_callback_(cfd, addr);
                }
            }
            continue;
        }
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
