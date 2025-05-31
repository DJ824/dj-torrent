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

    struct WorkAssignment {
        int piece_index;
        std::vector<std::pair<int, int>> blocks;

        WorkAssignment() : piece_index(-1) {}
        WorkAssignment(int piece, std::vector<std::pair<int, int>> block_list)
            : piece_index(piece), blocks(std::move(block_list)) {}

        bool has_work() const { return piece_index >= 0 && !blocks.empty(); }
        size_t block_count() const { return blocks.size(); }
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

    std::chrono::steady_clock::time_point last_regular_unchoke_;
    std::chrono::steady_clock::time_point last_optimistic_unchoke_;
    PeerConnection* current_optimistic_peer_;
    static constexpr std::chrono::seconds REGULAR_UNCHOKE_INTERVAL{10};
    static constexpr std::chrono::seconds OPTIMISTIC_UNCHOKE_INTERVAL{30};

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
    PieceManager* get_piece_manager() const { return piece_manager_; }
    void update_choking_decisions();


private:
    void management_loop();
    void maintain_connections();
    void connect_to_new_peers();
    void cleanup_dead_connections();
    void distribute_piece_requests();
    WorkAssignment get_work_for_peer(PeerConnection* peer);
    std::vector<Peer> select_best_peers_to_connect();d
    bool should_connect_to_peer(const Peer& peer) const;
    void update_stats();
    bool is_already_connected(const Peer& peer) const;
    void log_connection_attempt(const Peer& peer, bool success);
    void perform_regular_unchoke();
    void perform_optimistic_unchoke();

};