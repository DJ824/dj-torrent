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
#include <vector>

class Session {
public:
    Session(TorrentFile torrent,
            std::string peer_id,
            uint16_t listen_port,
            std::size_t block_size,
            std::filesystem::path download_path);

    void start();
    bool start_from_tracker();
    bool start_from_web_seeds();

    void add_peer(const PeerAddress& address);

    void run_once(int timeout_ms);
    void run(int timeout_ms);

    std::size_t peer_count() const;

private:
    struct PeerState {
        std::string remote_id;
        std::vector<uint8_t> bitfield;
        bool choked{true};
        bool interested{false};
        uint32_t inflight_requests{0};
    };

    void handle_peer_events(Peer& peer, std::vector<Peer::Event>&& events);
    void handle_piece_complete(uint32_t piece_index);
    void maybe_request(Peer& peer, PeerState& state);
    bool peer_has_interesting(const PeerState& state) const;
    PeerState& ensure_peer_state(int fd);
    uint32_t piece_length(uint32_t piece_index) const;
    bool try_download_from_web_seed(const std::string& base_url);
    std::string build_web_seed_url(const std::string& base) const;
    std::vector<std::string> collect_tracker_urls() const;
    static bool is_http_tracker(const std::string& url);

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
    AsyncLogger logger_;
};
