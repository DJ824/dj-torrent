#pragma once

#include "peer_event_loop.h"
#include "piece_manager.h"
#include "logger.h"
#include "storage.h"
#include "torrent_file.h"
#include "tracker_client.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>

class Session {
public:
    Session(TorrentFile torrent,
            std::string peer_id,
            uint16_t listen_port,
            std::size_t block_size,
            std::filesystem::path download_path);
    ~Session();

    void start();
    bool start_from_tracker();
    bool start_from_web_seeds();

    void add_peer(const PeerAddress& address);

    void run_once(int timeout_ms);
    void run(int timeout_ms);
    void stop();

    std::size_t peer_count() const;

private:
    struct PeerState {
        std::string remote_id;
        std::vector<uint8_t> bitfield;
        bool choked{true};
        bool interested{false};
        uint32_t inflight_requests{0};
        bool handshake_received{false};
        std::chrono::steady_clock::time_point connected_at{};
    };

    void handle_peer_events(Peer& peer, std::vector<Peer::Event>&& events);
    void handle_piece_complete(uint32_t piece_index);
    void maybe_request(Peer& peer, PeerState& state);
    bool peer_has_interesting(const PeerState& state) const;
    PeerState& ensure_peer_state(int fd);
    void connect_peer_now(const PeerAddress& address);
    void maybe_connect_pending_peers();
    void maybe_log_stats();
    void maybe_drop_handshake_timeouts();
    void handle_pex(Peer& from_peer, const std::vector<uint8_t>& payload);
    uint32_t piece_length(uint32_t piece_index) const;
    bool try_download_from_web_seed(const std::string& base_url);
    std::string build_web_seed_url(const std::string& base) const;
    std::vector<std::string> collect_tracker_urls() const;
    static bool is_http_tracker(const std::string& url);
    static bool is_udp_tracker(const std::string& url);
    void tracker_worker(std::vector<std::string> tracker_urls);
    bool enqueue_peer_candidate(const PeerAddress& address);
    void stop_tracker_thread();

    static std::vector<uint8_t> make_bitfield(std::size_t pieces);
    static void bitfield_set(std::vector<uint8_t>& bf, uint32_t idx);
    static bool bitfield_test(const std::vector<uint8_t>& bf, uint32_t idx);


    TorrentFile torrent_;
    std::string self_peer_id_;
    uint16_t listen_port_;
    std::size_t block_size_;
    TrackerClient tracker_client_;
    PieceManager piece_manager_;
    PeerEventLoop event_loop_;
    Storage storage_;
    std::unordered_map<int, PeerState> peers_;
    std::deque<PeerAddress> pending_peers_;
    std::unordered_set<std::string> known_endpoints_;
    std::mutex pending_mutex_;
    std::thread tracker_thread_;
    std::atomic<bool> tracker_stop_{false};
    std::atomic<bool> running_{true};
    std::chrono::steady_clock::time_point last_stats_log_{};
    std::chrono::steady_clock::time_point last_pex_broadcast_{};
    std::uint64_t pex_peers_discovered_{0};
    AsyncLogger logger_;
};
