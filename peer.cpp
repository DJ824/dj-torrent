#include "peer.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

#include "bencode.h"


static int make_nonblocking_socket(const PeerAddress& addr) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    std::string port_str = std::to_string(addr.port);
    addrinfo* res = nullptr;
    int rc = getaddrinfo(addr.ip.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerror(rc)));
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        throw std::runtime_error("Failed to create/connect socket");
    }
    return fd;
}

Peer::Peer(int fd, PeerAddress addr, std::array<uint8_t, 20> info_hash, std::string self_peer_id)
    : fd_(fd),
      remote_(std::move(addr)),
      info_hash_(info_hash),
      self_peer_id_(std::move(self_peer_id)) {}

Peer::~Peer() { close(); }

Peer::Peer(Peer&& other) noexcept { *this = std::move(other); }

Peer& Peer::operator=(Peer&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        remote_ = std::move(other.remote_);
        state_ = other.state_;
        info_hash_ = other.info_hash_;
        self_peer_id_ = std::move(other.self_peer_id_);
        remote_peer_id_ = std::move(other.remote_peer_id_);
        handshake_received_ = other.handshake_received_;
        handshake_sent_ = other.handshake_sent_;
        incoming_ = std::move(other.incoming_);
        outgoing_ = std::move(other.outgoing_);
        outgoing_offset_ = other.outgoing_offset_;
        events_ = std::move(other.events_);

        other.fd_ = -1;
        other.state_ = State::Closed;
        other.outgoing_offset_ = 0;
        other.incoming_.clear();
        other.outgoing_.clear();
        other.events_.clear();
    }
    return *this;
}

Peer Peer::connect_outgoing(const PeerAddress& addr,
                            const std::array<uint8_t, 20>& info_hash,
                            std::string self_peer_id) {
    int fd = make_nonblocking_socket(addr);
    Peer p(fd, addr, info_hash, std::move(self_peer_id));
    std::cout << "adding new peer with addr " << addr.ip << std::endl;
    p.state_ = State::Connecting;
    p.ensure_handshake_sent();
    return p;
}

Peer Peer::from_incoming(int fd,
                         const PeerAddress& addr,
                         const std::array<uint8_t, 20>& info_hash,
                         std::string self_peer_id) {
    Peer p(fd, addr, info_hash, std::move(self_peer_id));
    std::cout << "accepted incoming peer from addr " << addr.ip << std::endl;
    p.state_ = State::Handshaking;
    return p;
}

void Peer::close() {
    if (fd_ >= 0) {
        std::cerr << "closing peer connection to " << remote_.ip << ":" << remote_.port
                  << std::endl;
        ::close(fd_);
        fd_ = -1;
    }
    state_ = State::Closed;
    outgoing_.clear();
    incoming_.clear();
}

void Peer::handle_error() {
    std::cerr << "peer " << remote_.ip << ":" << remote_.port << " signaled error"
              << std::endl;
    close();
}

void Peer::handle_writable() {
    if (state_ == State::Connecting) {
        if (!check_socket_connected()) {
            std::cerr << "peer connect failed for " << remote_.ip << ":" << remote_.port
                      << std::endl;
            close();
            return;
        }
        state_ = State::Handshaking;
    }

    ensure_handshake_sent();

    while (!outgoing_.empty()) {
        std::vector<uint8_t>& front = outgoing_.front();
        std::size_t remaining = front.size() - outgoing_offset_;
        if (remaining == 0) {
            outgoing_.pop_front();
            outgoing_offset_ = 0;
            continue;
        }
        ssize_t n = ::send(fd_, front.data() + outgoing_offset_, remaining, 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return;
            }
            close();
            return;
        }
        outgoing_offset_ += static_cast<std::size_t>(n);
        if (outgoing_offset_ == front.size()) {
            outgoing_.pop_front();
            outgoing_offset_ = 0;
        }
    }
}

void Peer::handle_readable() {
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) break;
            close();
            return;
        }
        if (n == 0) {
            close();
            return;
        }
        incoming_.insert(incoming_.end(), buf, buf + n);
    }

    if (!handshake_received_) {
        if (!parse_handshake()) {
            return;
        }
    }
    if (handshake_received_) {
        parse_messages();
    }
}

bool Peer::parse_handshake() {
    std::cout << "parsing handshake" << std::endl;
    static constexpr uint8_t kPstrlen = 19;
    static constexpr std::string_view kPstr = "BitTorrent protocol";
    static constexpr std::size_t kHandshakeSize = 1 + kPstrlen + 8 + 20 + 20;
    if (incoming_.size() < kHandshakeSize) {
        return false;
    }

    if (incoming_[0] != kPstrlen) {
        close();
        return false;
    }
    if (std::string_view(reinterpret_cast<const char*>(&incoming_[1]), kPstrlen) != kPstr) {
        close();
        return false;
    }
    const uint8_t* reserved = &incoming_[1 + kPstrlen];
    (void)reserved;

    const uint8_t* info_hash = &incoming_[1 + kPstrlen + 8];
    if (!std::equal(info_hash, info_hash + 20, info_hash_.begin())) {
        close();
        return false;
    }
    const uint8_t* peer_id = info_hash + 20;
    remote_peer_id_.assign(reinterpret_cast<const char*>(peer_id), 20);

    events_.push_back(Event{EventType::Handshake, remote_peer_id_, {}, 0, 0, 0});

    incoming_.erase(incoming_.begin(), incoming_.begin() + static_cast<std::ptrdiff_t>(kHandshakeSize));
    handshake_received_ = true;
    if (state_ == State::Connecting) {
        state_ = State::Handshaking;
    }
    if (state_ == State::Handshaking) {
        state_ = State::Active;
    }
    return true;
}

void Peer::parse_messages() {
    while (incoming_.size() >= 4) {
        uint32_t msg_len = read_be32(incoming_.data());
        if (msg_len == 0) {
            events_.push_back(Event{EventType::KeepAlive, {}, {}, 0, 0, 0});
            incoming_.erase(incoming_.begin(), incoming_.begin() + 4);
            continue;
        }
        if (incoming_.size() < 4 + msg_len) {
            return;
        }
        uint8_t msg_id = incoming_[4];
        const uint8_t* payload = incoming_.data() + 5;
        uint32_t payload_len = msg_len - 1;

        switch (msg_id) {
            case 0:
                events_.push_back(Event{EventType::Choke, {}, {}, 0, 0, 0});
                break;
            case 1:
                events_.push_back(Event{EventType::Unchoke, {}, {}, 0, 0, 0});
                break;
            case 2:
                events_.push_back(Event{EventType::Interested, {}, {}, 0, 0, 0});
                break;
            case 3:
                events_.push_back(Event{EventType::NotInterested, {}, {}, 0, 0, 0});
                break;
            case 4: {
                if (payload_len == 4) {
                    uint32_t piece = read_be32(payload);
                    events_.push_back(Event{EventType::Have, {}, {}, piece, 0, 0});
                }
                break;
            }
            case 5: {
                std::vector<uint8_t> bf(payload, payload + payload_len);
                events_.push_back(Event{EventType::Bitfield, {}, std::move(bf), 0, 0, payload_len});
                break;
            }
            case 6: {
                if (payload_len == 12) {
                    uint32_t piece = read_be32(payload);
                    uint32_t begin = read_be32(payload + 4);
                    uint32_t len = read_be32(payload + 8);
                    events_.push_back(Event{EventType::Request, {}, {}, piece, begin, len});
                }
                break;
            }
            case 7: {
                if (payload_len >= 8) {
                    uint32_t piece = read_be32(payload);
                    uint32_t begin = read_be32(payload + 4);
                    std::vector<uint8_t> data(payload + 8, payload + payload_len);
                    events_.push_back(Event{EventType::Piece, {}, std::move(data), piece, begin,
                                            payload_len - 8});
                }
                break;
            }
            case 8: {
                if (payload_len == 12) {
                    uint32_t piece = read_be32(payload);
                    uint32_t begin = read_be32(payload + 4);
                    uint32_t len = read_be32(payload + 8);
                    events_.push_back(Event{EventType::Cancel, {}, {}, piece, begin, len});
                }
                break;
            }
            case 20: {
                if (payload_len >= 1) {
                    uint8_t ext_id = payload[0];
                    const uint8_t* ext_payload = payload + 1;
                    uint32_t ext_len = payload_len - 1;
                    if (ext_id == 0) {
                        std::vector<uint8_t> data(ext_payload, ext_payload + ext_len);
                        events_.push_back(Event{EventType::ExtendedHandshake, {}, std::move(data),
                                                0, 0, 0});
                        try {
                            std::string s(reinterpret_cast<const char*>(data.data()),
                                          data.size());
                            bencode::Parser parser(std::move(s));
                            bencode::Value v = parser.parse();
                            const auto& dict = bencode::as_dict(v);
                            if (const auto* m = bencode::find_field(dict, "m")) {
                                const auto& md = bencode::as_dict(*m);
                                if (const auto* utpex = bencode::find_field(md, "ut_pex")) {
                                    int64_t id = bencode::as_int(*utpex);
                                    if (id > 0 && id < 256) {
                                        remote_ut_pex_id_ = static_cast<uint8_t>(id);
                                    }
                                }
                            }
                        } catch (...) {
                        }
                    } else if (ext_id == remote_ut_pex_id_ && remote_ut_pex_id_ != 0) {
                        std::vector<uint8_t> data(ext_payload, ext_payload + ext_len);
                        events_.push_back(Event{EventType::Pex, {}, std::move(data), 0, 0, 0});
                    }
                }
                break;
            }
            default:
                break;
        }

        incoming_.erase(incoming_.begin(),
                        incoming_.begin() + static_cast<std::ptrdiff_t>(4 + msg_len));
    }
}

std::vector<Peer::Event> Peer::drain_events() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

void Peer::send_interested() { queue_bytes({0, 0, 0, 1, 2}); }
void Peer::send_not_interested() { queue_bytes({0, 0, 0, 1, 3}); }
void Peer::send_choke() { queue_bytes({0, 0, 0, 1, 0}); }
void Peer::send_unchoke() { queue_bytes({0, 0, 0, 1, 1}); }

void Peer::send_have(uint32_t piece_index) {
    std::vector<uint8_t> msg(9);
    write_be32(msg.data(), 5);
    msg[4] = 4;
    write_be32(msg.data() + 5, piece_index);
    queue_bytes(std::move(msg));
}

void Peer::send_request(uint32_t piece_index, uint32_t begin, uint32_t length) {
    std::vector<uint8_t> msg(17);
    write_be32(msg.data(), 13);
    msg[4] = 6;
    write_be32(msg.data() + 5, piece_index);
    write_be32(msg.data() + 9, begin);
    write_be32(msg.data() + 13, length);
    queue_bytes(std::move(msg));
}

void Peer::send_cancel(uint32_t piece_index, uint32_t begin, uint32_t length) {
    std::vector<uint8_t> msg(17);
    write_be32(msg.data(), 13);
    msg[4] = 8;
    write_be32(msg.data() + 5, piece_index);
    write_be32(msg.data() + 9, begin);
    write_be32(msg.data() + 13, length);
    queue_bytes(std::move(msg));
}

void Peer::send_bitfield(const std::vector<uint8_t>& bitfield) {
    std::vector<uint8_t> msg(5 + bitfield.size());
    write_be32(msg.data(), static_cast<uint32_t>(1 + bitfield.size()));
    msg[4] = 5;
    std::copy(bitfield.begin(), bitfield.end(), msg.begin() + 5);
    queue_bytes(std::move(msg));
}

void Peer::send_piece(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> msg(13 + data.size());
    write_be32(msg.data(), static_cast<uint32_t>(9 + data.size()));
    msg[4] = 7;
    write_be32(msg.data() + 5, piece_index);
    write_be32(msg.data() + 9, begin);
    std::copy(data.begin(), data.end(), msg.begin() + 13);
    queue_bytes(std::move(msg));
}

void Peer::send_extended_handshake() {
    if (extended_handshake_sent_) {
        return;
    }
    std::string payload = "d1:md6:ut_pexi";
    payload += std::to_string(static_cast<int>(kLocalUtPexId_));
    payload += "ee";

    uint32_t ext_len = static_cast<uint32_t>(1 + payload.size());
    uint32_t msg_len = 1 + ext_len;
    std::vector<uint8_t> msg(4 + msg_len);
    write_be32(msg.data(), msg_len);
    msg[4] = 20;
    msg[5] = 0;
    std::memcpy(msg.data() + 6, payload.data(), payload.size());
    queue_bytes(std::move(msg));
    extended_handshake_sent_ = true;
}

void Peer::send_ut_pex(const std::vector<PeerAddress>& added) {
    if (added.empty()) {
        return;
    }
    std::string compact;
    compact.reserve(6 * added.size());
    for (const auto& a : added) {
        in_addr addr{};
        if (inet_pton(AF_INET, a.ip.c_str(), &addr) != 1) {
            continue;
        }
        compact.append(reinterpret_cast<const char*>(&addr), sizeof(addr));
        uint16_t port_be = htons(a.port);
        compact.append(reinterpret_cast<const char*>(&port_be), sizeof(port_be));
    }
    if (compact.empty()) {
        return;
    }

    std::string payload = "d5:added";
    payload += std::to_string(compact.size());
    payload += ":";
    payload += compact;
    payload += "e";

    uint32_t ext_len = static_cast<uint32_t>(1 + payload.size());
    uint32_t msg_len = 1 + ext_len;
    std::vector<uint8_t> msg(4 + msg_len);
    write_be32(msg.data(), msg_len);
    msg[4] = 20;
    msg[5] = kLocalUtPexId_;
    std::memcpy(msg.data() + 6, payload.data(), payload.size());
    queue_bytes(std::move(msg));
}

void Peer::queue_bytes(std::vector<uint8_t> bytes) {
    outgoing_.push_back(std::move(bytes));
}

void Peer::ensure_handshake_sent() {
    if (handshake_sent_) {
        return;
    }
    // std::cout << "sending handshake" << std::endl;
    queue_bytes(make_handshake(info_hash_, self_peer_id_));
    handshake_sent_ = true;
}

bool Peer::check_socket_connected() {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return false;
    }
    return err == 0;
}

std::vector<uint8_t> Peer::make_handshake(const std::array<uint8_t, 20>& info_hash,
                                          const std::string& peer_id) {
    static constexpr uint8_t kPstrlen = 19;
    static constexpr std::string_view kPstr = "BitTorrent protocol";
    if (peer_id.size() != 20) {
        throw std::runtime_error("peer_id must be 20 bytes");
    }
    std::vector<uint8_t> msg(1 + kPstrlen + 8 + 20 + 20, 0);
    msg[0] = kPstrlen;
    std::copy(kPstr.begin(), kPstr.end(), msg.begin() + 1);
    std::copy(info_hash.begin(), info_hash.end(), msg.begin() + 1 + kPstrlen + 8);
    std::copy(peer_id.begin(), peer_id.end(), msg.begin() + 1 + kPstrlen + 8 + 20);
    return msg;
}

uint32_t Peer::read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void Peer::write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}
