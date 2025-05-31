#include "peer_manager.h"
#include "peer_connection.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <random>

PeerManager::PeerManager(PieceManager* piece_manager, TorrentFile* torrent,
                        const std::string& peer_id)
    : piece_manager_(piece_manager), torrent_(torrent), peer_id_(peer_id),
      running_(false), last_stats_update_(std::chrono::steady_clock::now()),
      current_optimistic_peer_(nullptr) {

    if (!piece_manager || !torrent) {
        throw std::invalid_argument("piece_manager and torrent cannot be null");
    }

    auto now = std::chrono::steady_clock::now();
    last_regular_unchoke_ = now;
    last_optimistic_unchoke_ = now;

    std::cout << "peer manager initialized for torrent with "
              << torrent_->get_piece_hashes().size() << " pieces" << std::endl;
}

PeerManager::~PeerManager() {
    stop();
}

void PeerManager::start() {
    if (running_.exchange(true)) {
        std::cout << "peer manager already running" << std::endl;
        return;
    }

    std::cout << "starting peer manager..." << std::endl;
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

    std::cout << "added " << new_peers
              << " new peers, total pending: " << pending_peers_.size()
              << std::endl;
    if (new_peers > 0) {
        std::cout << "notifying peer manager, new peers available" << std::endl;
        work_available_.notify_one();
    }
}

void PeerManager::remove_failed_peer(PeerConnection* peer) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = std::find_if(connections_.begin(), connections_.end(),
                          [peer](const std::unique_ptr<PeerConnection>& conn) {
                              return conn.get() == peer;
                          });

    if (it != connections_.end()) {
        std::cout << "removing failed peer connection: " << peer->get_peer().to_string() << std::endl;

        if (current_optimistic_peer_ == peer) {
            current_optimistic_peer_ = nullptr;
        }

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
    std::cout << "\r[peers] active: " << stats.active_connections << "/"
              << MAX_CONNECTIONS << " | progress: " << std::fixed
              << std::setprecision(1) << completion
              << "% | rate: " << std::setprecision(2)
              << (stats.download_rate / 1024.0) << " kb/s" << std::flush;
}

void PeerManager::notify_piece_completed(int piece_index) {
    std::cout << "\npiece " << piece_index << " completed by peer" << std::endl;
    work_available_.notify_one();
}

void PeerManager::request_more_work(PeerConnection* peer) {
    auto work_assignment = get_work_for_peer(peer);
    if (work_assignment.has_work()) {
        std::cout << "assigning piece " << work_assignment.piece_index
                  << " with " << work_assignment.block_count()
                  << " blocks to " << peer->get_peer().to_string() << std::endl;

        for (const auto& [offset, length] : work_assignment.blocks) {
            if (!peer->send_request(work_assignment.piece_index, offset, length)) {
                std::cerr << "failed to send request to " << peer->get_peer().to_string() << std::endl;
                break;
            }
        }
    } else {
        std::cout << "no work available for peer " << peer->get_peer().to_string() << std::endl;
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
            update_choking_decisions();
            update_stats();
            last_maintenance = now;
        }

        std::unique_lock<std::mutex> lock(connections_mutex_);
        auto wait_result = work_available_.wait_for(lock, MAINTENANCE_INTERVAL);

        if (wait_result == std::cv_status::no_timeout) {
            std::cout << "peer manager woken by notification" << std::endl;
        } else {
            std::cout << "peer manager woken by timeout (5s)" << std::endl;
        }
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
        auto& peer = best_peers[i];
        if (should_connect_to_peer(peer)) {
            try {
                auto connection = std::make_unique<PeerConnection>(peer, this, torrent_, peer_id_);

                if (connection->connect()) {
                    std::cout << "successfully connected to " << peer.to_string() << std::endl;
                    connections_.push_back(std::move(connection));
                    stats_.total_peers_tried++;
                    log_connection_attempt(peer, true);
                } else {
                    std::cout << "failed to establish connection to " << peer.to_string() << std::endl;
                    peer.mark_connection_failure();
                    stats_.failed_connections++;
                    log_connection_attempt(peer, false);
                }

                auto it = std::find_if(pending_peers_.begin(), pending_peers_.end(),
                                      [&peer](const Peer& p) { return p == peer; });
                if (it != pending_peers_.end()) {
                    pending_peers_.erase(it);
                }

            } catch (const std::exception& e) {
                std::cerr << "exception while connecting to " << peer.to_string()
                          << " - " << e.what() << std::endl;
                peer.mark_connection_failure();
                stats_.failed_connections++;
                log_connection_attempt(peer, false);
            }
        }
    }
}

void PeerManager::cleanup_dead_connections() {
    auto it = connections_.begin();
    while (it != connections_.end()) {
        if (!(*it)->is_connected()) {
            std::cout << "removing dead connection to " << (*it)->get_peer().to_string() << std::endl;

            if (current_optimistic_peer_ == it->get()) {
                current_optimistic_peer_ = nullptr;
            }

            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void PeerManager::distribute_piece_requests() {
    if (piece_manager_->is_complete()) {
        return;
    }

    for (auto& conn : connections_) {
        if (conn->is_connected() && conn->is_handshake_complete()) {
            auto state = conn->get_state();
            if (!state.peer_choking && conn->get_pending_request_count() < 5) {
                request_more_work(conn.get());
            }
        }
    }
}

PeerManager::WorkAssignment PeerManager::get_work_for_peer(PeerConnection* peer) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        int piece_index = piece_manager_->get_next_needed_piece();
        if (piece_index < 0) {
            std::cout << "no more pieces needed" << std::endl;
            return WorkAssignment();
        }

        if (!peer->peer_has_piece(piece_index)) {
            std::cout << "peer " << peer->get_peer().to_string()
                      << " doesn't have piece " << piece_index << ", trying another" << std::endl;
            continue;
        }

        auto missing_blocks = piece_manager_->get_missing_blocks(piece_index);
        if (missing_blocks.empty()) {
            std::cout << "piece " << piece_index << " has no missing blocks, trying next" << std::endl;
            continue;
        }

        size_t blocks_to_assign = std::min(missing_blocks.size(), size_t(5));
        std::vector<std::pair<int, int>> selected_blocks;
        for (size_t i = 0; i < blocks_to_assign; ++i) {
            selected_blocks.push_back(missing_blocks[i]);
        }

        if (piece_manager_->request_piece(piece_index)) {
            std::cout << "selected piece " << piece_index << " with "
                      << selected_blocks.size() << " blocks for "
                      << peer->get_peer().to_string() << std::endl;
            return WorkAssignment(piece_index, std::move(selected_blocks));
        } else {
            std::cout << "failed to request piece " << piece_index << ", trying next" << std::endl;
        }
    }

    std::cout << "could not find work after 10 attempts for "
              << peer->get_peer().to_string() << std::endl;
    return WorkAssignment();
}

void PeerManager::update_choking_decisions() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto now = std::chrono::steady_clock::now();

    if (now - last_regular_unchoke_ >= REGULAR_UNCHOKE_INTERVAL) {
        perform_regular_unchoke();
        last_regular_unchoke_ = now;
    }

    if (now - last_optimistic_unchoke_ >= OPTIMISTIC_UNCHOKE_INTERVAL) {
        perform_optimistic_unchoke();
        last_optimistic_unchoke_ = now;
    }
}

void PeerManager::perform_regular_unchoke() {
    std::cout << "\n=== regular unchoke (rewarding best uploaders) ===" << std::endl;

    std::vector<PeerConnection*> interested_peers;
    for (auto& conn : connections_) {
        if (conn->is_connected() && conn->is_handshake_complete()) {
            auto state = conn->get_state();
            if (state.peer_interested) {
                interested_peers.push_back(conn.get());
            }
        }
    }

    std::sort(interested_peers.begin(), interested_peers.end(),
        [](PeerConnection* a, PeerConnection* b) {
            return a->get_peer().bytes_downloaded_ > b->get_peer().bytes_downloaded_;
        });

    size_t unchoked = 0;
    for (auto* peer_conn : interested_peers) {
        if (unchoked < 3 && peer_conn != current_optimistic_peer_) {
            if (peer_conn->send_unchoke()) {
                unchoked++;
                std::cout << "regular unchoke: " << peer_conn->get_peer().to_string()
                          << " (uploaded " << peer_conn->get_peer().bytes_downloaded_
                          << " bytes to us)" << std::endl;
            }
        } else {
            peer_conn->send_choke();
        }
    }
    std::cout << "regular unchoked " << unchoked << " peers" << std::endl;
}

void PeerManager::perform_optimistic_unchoke() {
    std::cout << "\n=== optimistic unchoke (trying new peer) ===" << std::endl;

    if (current_optimistic_peer_) {
        if (current_optimistic_peer_->send_choke()) {
            std::cout << "choked previous optimistic peer: "
                      << current_optimistic_peer_->get_peer().to_string() << std::endl;
        }
    }

    std::vector<PeerConnection*> candidates;
    for (auto& conn : connections_) {
        if (conn->is_connected() && conn->is_handshake_complete()) {
            auto state = conn->get_state();
            if (state.peer_interested && state.am_choking) {
                candidates.push_back(conn.get());
            }
        }
    }

    if (!candidates.empty()) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, candidates.size() - 1);

        current_optimistic_peer_ = candidates[dis(gen)];
        if (current_optimistic_peer_->send_unchoke()) {
            std::cout << "new optimistic unchoke: "
                      << current_optimistic_peer_->get_peer().to_string() << std::endl;
        } else {
            current_optimistic_peer_ = nullptr;
        }
    } else {
        current_optimistic_peer_ = nullptr;
        std::cout << "no candidates for optimistic unchoke" << std::endl;
    }
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
    return std::any_of(connections_.begin(), connections_.end(),
        [&peer](const std::unique_ptr<PeerConnection>& conn) {
            return conn->get_peer() == peer;
        });
}

void PeerManager::log_connection_attempt(const Peer& peer, bool success) {
    if (success) {
        std::cout << "successfully connected to " << peer.to_string() << std::endl;
    } else {
        std::cout << "failed to connect to " << peer.to_string() << std::endl;
    }
}