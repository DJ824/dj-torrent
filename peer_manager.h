#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "piece_manager.h"
#include "torrent_file.h"
#include "peer.h"

class PeerConnection;

class PeerManager {
public:
    struct PeerStats {
        size_t active_connections = 0;
        size_t total_peers_tried = 0;
        size_t failed_connections = 0;
        double download_rate = 0.0;
    };

private:
    PieceManager* piece_manager_;
    TorrentFile* torrent_;
    std::string peer_id_;

    std::vector<std::unique_ptr<PeerConnection>> connections_;
    std::vector<Peer> pending_peers_;
    mutable std::mutex connections_mutex_;
    std::thread manager_thread_;
    std::atomic<bool> running_;
    std::condition_variable work_available_;
    static constexpr size_t MAX_CONNECTIONS = 30;
    static constexpr size_t MIN_CONNECTIONS = 10;
    static constexpr std::chrono::seconds CONNECTION_TIMEOUT{30};
    static constexpr std::chrono::seconds MAINTENANCE_INTERVAL{5};

    PeerStats stats_;
    std::chrono::steady_clock::time_point last_stats_update_;

public:
    explicit PeerManager(PieceManager* piece_manager, TorrentFile* torrent, const std::string& peer_id);
    ~PeerManager();

    void start();
    void stop();
    bool is_running() const { return running_; }
    void add_peers(const std::vector<Peer>& peers);
    void remove_failed_peer(PeerConnection* peer);
    PeerStats get_stats() const;
    void print_status() const;
    void notify_piece_completed(int piece_index);
    void request_more_work(PeerConnection* peer);

private:
    void management_loop();
    void maintain_connections();
    void connect_to_new_peers();
    void cleanup_dead_connections();
    void distribute_piece_requests();
    std::vector<std::pair<int, int>> get_work_for_peer(PeerConnection* peer);
    std::vector<Peer> select_best_peers_to_connect();
    bool should_connect_to_peer(const Peer& peer) const;
    void update_stats();
    bool is_already_connected(const Peer& peer) const;
    void log_connection_attempt(const Peer& peer, bool success);


};