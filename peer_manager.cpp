#include "peer_manager.h"
#include "peer_manager.h"
#include <iostream>
#include <algorithm>
#include <iomanip>

PeerManager::PeerManager(PieceManager* piece_manager, TorrentFile* torrent,
                        const std::string& peer_id)
    : piece_manager_(piece_manager), torrent_(torrent), peer_id_(peer_id),
      running_(false), last_stats_update_(std::chrono::steady_clock::now()) {

    if (!piece_manager || !torrent) {
        throw std::invalid_argument("piece_manager and torrent cannot be null");
    }

    std::cout << "PeerManager initialized for torrent with "
              << torrent_->get_piece_hashes().size() << " pieces" << std::endl;
}

PeerManager::~PeerManager() { stop(); }

void PeerManager::start() {
    if (running_.exchange(true)) {
        std::cout << "PeerManager already running" << std::endl;
        return;
    }

    std::cout << "starting PeerManager..." << std::endl;
    manager_thread_ = std::thread(&PeerManager::management_loop, this);
}

void PeerManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    std::cout << "stopping peer manager" << std::endl;

    work_available_.notify_all();
    if (manager_thread_.joinable()) {
        manager_thread_.join();
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.clear();
    std::cout << "peer manager stopped" << std::endl;
}

void PeerManager::add_peers(const std::vector<Peer>& peers) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    size_t new_peers = 0;
    for (const auto& peer : peers) {
        if (!is_already_connected(peer) && peer.is_usable()) {
            pending_peers_.push_back(peer);
            ++new_peers;
        }
    }

    std::cout << "added " << new_peers << " new peers, total pending: " << pending_peers_.size() << std::endl;
    work_available_.notify_one();
}

void PeerManager::remove_failed_peer(PeerConnection* peer) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = std::find_if(connections_.begin(), connections_.end(), [peer](const std::unique_ptr<PeerConnection>& conn) {
        return conn.get() == peer;
    });

    if (it != connections_.end()) {
        std::cout << "removing failed peer connection" << std::endl;
        connections_.erase(it);
        stats_.failed_connections++;
    }
}

PeerManager::PeerStats PeerManager::get_stats() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return stats_;
}

void PeerManager::print_status() const {
    auto stats = get_stats();
    double completion = piece_manager_->get_completion_percentage();
    std::cout << "\r[Peers] Active: " << stats.active_connections
              << "/" << MAX_CONNECTIONS
              << " | Progress: " << std::fixed << std::setprecision(1)
              << completion << "% | Rate: "
              << std::setprecision(2) << (stats.download_rate / 1024.0) << " KB/s"
              << std::flush;
}

void PeerManager::notify_piece_completed(int piece_index) {
    std::cout << "\nPiece " << piece_index << " completed by peer" << std::endl;
    work_available_.notify_one();
}

void PeerManager::request_more_work(PeerConnection* peer) {
    auto work = get_work_for_peer(peer);
    if (!work.empty()) {
        // todo peer connection

    }
}


void PeerManager::management_loop() {
    std::cout << "peer manager management loop started" << std::endl;
    auto last_maintenance = std::chrono::steady_clock::now();
    while (running_) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_maintenance >= MAINTENANCE_INTERVAL) {
            maintain_connections();
            distribute_piece_requests();
            update_stats();
            last_maintenance = now;
        }

        std::unique_lock<std::mutex> lock(connections_mutex_);
        work_available_.wait_for(lock, MAINTENANCE_INTERVAL);
    }

    std::cout << "peer manager management loop finished" << std::endl;
}

void PeerManager::maintain_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    cleanup_dead_connections();
    if (connections_.size() < MIN_CONNECTIONS && !pending_peers_.empty()) {
        connect_to_new_peers();
    }
}

void PeerManager::connect_to_new_peers() {
    size_t available_slots = MAX_CONNECTIONS - connections_.size();
    size_t peers_to_connect = std::min(available_slots, pending_peers_.size());
    if (peers_to_connect == 0) {
        return;
    }
    std::cout << "attempting to connect to " << peers_to_connect << " new peers" << std::endl;

    auto best_peers = select_best_peers_to_connect();

    for (size_t i = 0; i < std::min(peers_to_connect, best_peers.size()); ++i) {
        const auto& peer = best_peers[i];
        if (should_connect_to_peer(peer)) {
            try {
                auto& mutable_peer = const_cast<Peer&>(peer);
                mutable_peer.mark_connection_attempt();
                // todo peer connection

                std::cout << "would connect to " << peer.to_string() << std::endl;
                stats_.total_peers_tried++;

                auto it = std::find_if(pending_peers_.begin(), pending_peers_.end(),
                    [&peer](const Peer& p) { return p == peer; });
                if (it != pending_peers_.end()) {
                    pending_peers_.erase(it);
                }

            } catch (const std::exception& e) {
                std::cerr << "failed to connect to " << peer.to_string() << " - " << e.what() << std::endl;
                auto& mutable_peer = const_cast<Peer&>(peer);
                mutable_peer.mark_connection_failure();
                stats_.failed_connections++;
                log_connection_attempt(peer, false);
            }
        }
    }
}

void PeerManager::cleanup_dead_connections() {
    // todo peer connection
}

void PeerManager::distribute_piece_requests() {
    if (piece_manager_->is_complete()) {
        return;
    }

    // todo peer connections
}

std::vector<std::pair<int, int>> PeerManager::get_work_for_peer(PeerConnection* peer) {
    std::vector<std::pair<int, int>> work;

    int piece_index = piece_manager_->get_next_needed_piece();
    if (piece_index >= 0) {
        // todo peer connection

        auto missing_blocks = piece_manager_->get_missing_blocks(piece_index);
        size_t blocks_to_assign = std::min(missing_blocks.size(), size_t(5));

        for (size_t i = 0; i < blocks_to_assign; ++i) {
            work.push_back(missing_blocks[i]);
        }

        if (!work.empty()) {
            piece_manager_->request_piece(piece_index);
        }
    }

    return work;
}

std::vector<Peer> PeerManager::select_best_peers_to_connect() {
    std::sort(pending_peers_.begin(), pending_peers_.end());
    std::vector<Peer> good_peers;
    for (auto& peer : pending_peers_) {
        if (peer.should_retry_connection()) {
            good_peers.push_back(peer);
        }
    }
    return good_peers;
}

bool PeerManager::should_connect_to_peer(const Peer& peer) const {
    return peer.is_usable() && peer.should_retry_connection();
}


void PeerManager::update_stats() {
    auto now = std::chrono::steady_clock::now();
    auto time_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_stats_update_).count();

    if (time_elapsed > 0) {
        size_t bytes_downloaded = piece_manager_->get_bytes_downloaded();
        stats_.download_rate = static_cast<double>(bytes_downloaded) / time_elapsed;

        last_stats_update_ = now;
    }

    stats_.active_connections = connections_.size();
}

bool PeerManager::is_already_connected(const Peer& peer) const {
    return std::any_of(connections_.begin(), connections_.end(), [&peer](const std::unique_ptr<PeerConnection>& conn) {
        // todo peer connection
            return false;
    });
}


void PeerManager::log_connection_attempt(const Peer& peer, bool success) {
    if (success) {
        std::cout << "successfully connected to " << peer.to_string() << std::endl;
    } else {
        std::cout << "failed to connect to " << peer.to_string() << std::endl;
    }
}

