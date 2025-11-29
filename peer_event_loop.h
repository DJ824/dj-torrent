#pragma once

#include "peer.h"
#include <functional>
#include <unordered_map>

class PeerEventLoop {
public:
    using EventCallback = std::function<void(Peer&, std::vector<Peer::Event>&&)>;
    using AcceptCallback = std::function<void(int fd, const PeerAddress& addr)>;

    explicit PeerEventLoop(EventCallback cb);
    ~PeerEventLoop();

    PeerEventLoop(const PeerEventLoop&) = delete;
    PeerEventLoop& operator=(const PeerEventLoop&) = delete;
    PeerEventLoop(PeerEventLoop&&) = delete;
    PeerEventLoop& operator=(PeerEventLoop&&) = delete;

    bool add_peer(Peer peer);
    bool set_listen_socket(int fd, AcceptCallback cb);
    void remove_peer(int fd);
    void run_once(int timeout_ms);
    void run(int timeout_ms);
    void stop();
    std::size_t peer_count() const { return peers_.size(); }
    Peer* peer_by_fd(int fd);
    void for_each_peer(const std::function<void(Peer&)>& fn);

private:

    struct Entry {
        Peer peer;
        uint32_t events;
    };

    void update_interest(int fd, Entry& entry);

    int epfd_{-1};
    EventCallback callback_;
    std::unordered_map<int, Entry> peers_;
    int listen_fd_{-1};
    AcceptCallback accept_callback_;
    bool running_{false};
};
