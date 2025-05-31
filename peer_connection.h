#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <queue>
#include <bitset>
#include <sys/socket.h>
#include "peer.h"
#include "torrent_file.h"

class PeerManager;

class PeerConnection {
public:
    enum MessageType : uint8_t {
        CHOKE = 0,
        UNCHOKE = 1,
        INTERESTED = 2,
        NOT_INTERESTED = 3,
        HAVE = 4,
        BITFIELD = 5,
        REQUEST = 6,
        PIECE = 7,
        CANCEL = 8,
        PORT = 9
    };

    struct PeerState {
        bool am_choking = true;
        bool am_interested = false;
        bool peer_choking = true;
        bool peer_interested = false;
    };

    struct PendingRequest {
        uint32_t piece_index;
        uint32_t block_offset;
        uint32_t block_length;
        std::chrono::steady_clock::time_point request_time;

        PendingRequest(uint32_t piece, uint32_t offset, uint32_t length)
            : piece_index(piece), block_offset(offset), block_length(length), request_time(std::chrono::steady_clock::now()) {}


    };

private:
    Peer& peer_;
    PeerManager* peer_manager_;
    TorrentFile* torrent_;
    std::string our_peer_id_;

    int socket_fd_;
    std::atomic<bool> connected_;
    std::atomic<bool> running_;

    PeerState state_;
    // max of 32k pieces
    std::bitset<32768> peer_bitfield_;
    bool handshake_complete_;

    std::thread receive_thread_;
    mutable std::mutex state_mutex_;
    mutable std::mutex send_mutex_;

    std::queue<PendingRequest> pending_requests_;
    static constexpr size_t MAX_PENDING_REQUESTS = 10;
    static constexpr std::chrono::seconds REQUEST_TIMEOUT{30};

    std::chrono::steady_clock::time_point connection_start_;
    size_t bytes_sent_;
    size_t bytes_received_;

public:
    PeerConnection(Peer& peer, PeerManager* manager, TorrentFile* torrent, const std::string& peer_id);
    ~PeerConnection();

    bool connect();
    void disconnect();
    bool is_connected() const { return connected_; }
    bool is_handshake_complete() const { return handshake_complete_; }

    bool send_interested();
    bool send_not_interested();
    bool send_choke();
    bool send_unchoke();
    bool send_bitfield();
    bool send_request(uint32_t piece, uint32_t offset, uint32_t length);
    bool send_cancel(uint32_t piece, uint32_t offset, uint32_t length);
    bool send_have(uint32_t piece_index);

    const Peer& get_peer() const { return peer_; }
    PeerState get_state() const;
    bool peer_has_piece(uint32_t piece_index) const;
    size_t get_available_piece_count() const;

    double get_download_rate() const;
    size_t get_pending_request_count() const;
    std::chrono::seconds get_connection_duration() const;

private:
    bool perform_handshake();
    bool send_handshake();
    bool receive_handshake();

    void receive_loop();
    bool receive_message();
    bool send_message(const std::vector<uint8_t>& message);

    void handle_choke();
    void handle_unchoke();
    void handle_interested();
    void handle_not_interested();
    void handle_have(uint32_t piece_index);
    void handle_bitfield(const std::vector<uint8_t>& bitfield_data);
    void handle_request(uint32_t piece, uint32_t offset, uint32_t length);
    void handle_piece(uint32_t piece, uint32_t offset, const std::vector<uint8_t>& data);
    void handle_cancel(uint32_t piece, uint32_t offset, uint32_t length);

    std::vector<uint8_t> create_handshake_message();
    bool send_message_immediate(const std::vector<uint8_t> &message);
    std::vector<uint8_t> create_message(MessageType type);
    std::vector<uint8_t> create_cancel_message(uint32_t piece, uint32_t uint32, uint32_t length);
    std::vector<uint8_t> create_have_message(uint32_t piece_index);
    std::vector<uint8_t> create_request_message(uint32_t piece_index, uint32_t offset, uint32_t length);
    std::vector<uint8_t> create_piece_message(uint32_t piece, uint32_t offset, const std::vector<uint8_t>& data);

    bool send_raw_data(const uint8_t* data, size_t length);
    std::vector<uint8_t> receive_raw_data(size_t length);
    void cleanup_expired_requests();
    void update_interest_state();
    bool should_be_interested() const;

    bool create_socket();
    void close_socket();
};