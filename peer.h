#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

struct PeerAddress {
    std::string ip;
    uint16_t port{};
};

class Peer {
public:
    enum class State { Connecting, Handshaking, Active, Closed };

    enum class EventType {
        Handshake,
        KeepAlive,
        Choke,
        Unchoke,
        Interested,
        NotInterested,
        Have,
        Bitfield,
        Request,
        Piece,
        Cancel,
    };

    struct Event {
        EventType type;
        std::string peer_id;
        std::vector<uint8_t> payload;
        uint32_t piece_index{0};
        uint32_t begin{0};
        uint32_t length{0};
    };

    static Peer connect_outgoing(const PeerAddress& addr,
                                 const std::array<uint8_t, 20>& info_hash,
                                 std::string self_peer_id);

    ~Peer();
    Peer(Peer&&) noexcept;
    Peer& operator=(Peer&&) noexcept;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    int fd() const { return fd_; }
    State state() const { return state_; }
    bool is_closed() const { return state_ == State::Closed; }
    bool wants_write() const { return !outgoing_.empty(); }
    const PeerAddress& remote() const { return remote_; }

    void handle_readable();
    void handle_writable();
    void handle_error();

    std::vector<Event> drain_events();

    void send_interested();
    void send_not_interested();
    void send_choke();
    void send_unchoke();
    void send_have(uint32_t piece_index);
    void send_request(uint32_t piece_index, uint32_t begin, uint32_t length);
    void send_cancel(uint32_t piece_index, uint32_t begin, uint32_t length);
    void send_bitfield(const std::vector<uint8_t>& bitfield);
    void send_piece(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& data);

private:
    Peer(int fd, PeerAddress addr, std::array<uint8_t, 20> info_hash, std::string self_peer_id);

    void close();
    void queue_bytes(std::vector<uint8_t> bytes);
    void ensure_handshake_sent();
    bool parse_handshake();
    void parse_messages();
    bool check_socket_connected();

    static std::vector<uint8_t> make_handshake(const std::array<uint8_t, 20>& info_hash,
                                               const std::string& peer_id);
    static uint32_t read_be32(const uint8_t* p);
    static void write_be32(uint8_t* p, uint32_t v);

    int fd_{-1};
    PeerAddress remote_{};
    State state_{State::Connecting};
    std::array<uint8_t, 20> info_hash_{};
    std::string self_peer_id_;
    std::string remote_peer_id_;
    bool handshake_received_{false};
    bool handshake_sent_{false};

    std::vector<uint8_t> incoming_;
    std::deque<std::vector<uint8_t>> outgoing_;
    std::size_t outgoing_offset_{0};

    std::vector<Event> events_;
};
