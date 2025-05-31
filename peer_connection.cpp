#include <iostream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <iomanip>
#include "peer_connection.h"
#include "peer_manager.h"

PeerConnection::PeerConnection(Peer& peer, PeerManager* manager,
                              TorrentFile* torrent, const std::string& peer_id)
    : peer_(peer), peer_manager_(manager), torrent_(torrent), our_peer_id_(peer_id),
      socket_fd_(-1), connected_(false), running_(false),
      handshake_complete_(false), connection_start_(std::chrono::steady_clock::now()),
      bytes_sent_(0), bytes_received_(0) {

    if (!manager || !torrent) {
        throw std::invalid_argument("manager and torrent cannot be null");
    }

    if (peer_id.length() != 20) {
        throw std::invalid_argument("peer_id must be exactly 20 bytes");
    }
}

PeerConnection::~PeerConnection() {
    disconnect();
}

bool PeerConnection::connect() {
    if (connected_) {
        return true;
    }

    std::cout << "connecting to peer " << peer_.to_string() << std::endl;
    peer_.mark_connection_attempt();

    if (!create_socket()) {
        peer_.mark_connection_failure();
        return false;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(peer_.port_);

    if (inet_pton(AF_INET, peer_.ip_.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "invalid ip address: " << peer_.ip_ << std::endl;
        close_socket();
        peer_.mark_connection_failure();
        return false;
    }

    if (::connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "failed to connect to " << peer_.to_string() << " - " << strerror(errno) << std::endl;
        close_socket();
        peer_.mark_connection_failure();
        return false;
    }

    connected_ = true;
    running_ = true;
    peer_.mark_connection_success();
    peer_.state = Peer::CONNECTED;

    if (!perform_handshake()) {
        std::cerr << "handshake failed with " << peer_.to_string() << std::endl;
        disconnect();
        return false;
    }

    receive_thread_ = std::thread(&PeerConnection::receive_loop, this);
    std::cout << "connected to " << peer_.to_string() << std::endl;
    return true;
}

void PeerConnection::disconnect() {
    if (!connected_) {
        return;
    }

    std::cout << "disconnecting from " << peer_.to_string() << std::endl;
    running_ = false;
    connected_ = false;
    peer_.state = Peer::DISCONNECTED;

    close_socket();
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    std::cout << "disconnected from " << peer_.to_string() << std::endl;
}

bool PeerConnection::perform_handshake() {
    if (!send_handshake()) {
        return false;
    }

    if (!receive_handshake()) {
        return false;
    }

    handshake_complete_ = true;
    peer_.state = Peer::HANDSHAKED;
    std::cout << "handshake completed with " << peer_.to_string() << std::endl;

    if (peer_manager_->get_piece_manager()->get_completion_percentage() > 0) {
        send_bitfield();
    }
    return true;
}

bool PeerConnection::send_handshake() {
    auto handshake = create_handshake_message();
    return send_raw_data(handshake.data(), handshake.size());
}

bool PeerConnection::receive_handshake() {
    auto length_data = receive_raw_data(1);
    if (length_data.empty() || length_data[0] != 19) {
        std::cerr << "invalid handshake protocol length, expected 19" << std::endl;
        return false;
    }

    auto protocol_data = receive_raw_data(19);
    if (protocol_data.empty()) {
        std::cerr << "failed to read protocol string" << std::endl;
        return false;
    }

    std::string protocol_string(protocol_data.begin(), protocol_data.end());
    if (protocol_string != "BitTorrent protocol") {
        std::cerr << "invalid protocol string, expected 'BitTorrent protocol', got " << protocol_string << std::endl;
        return false;
    }

    auto reserved = receive_raw_data(8);
    if (reserved.empty()) {
        std::cerr << "failed to read reserved bytes" << std::endl;
        return false;
    }

    if (reserved[5] & 0x10) {
        peer_.supports_fast_extension_ = true;
        std::cout << "peer supports fast extension" << std::endl;
    }

    if (reserved[7] & 0x01) {
        peer_.supports_dht_ = true;
        std::cout << "peer supports dht" << std::endl;
    }

    auto info_hash = receive_raw_data(20);
    if (info_hash.empty()) {
        std::cerr << "failed to read info hash" << std::endl;
        return false;
    }

    std::string received_hash(info_hash.begin(), info_hash.end());
    const std::string& expected_hash = torrent_->get_info_hash();

    if (received_hash != expected_hash) {
        std::cerr << "info hash mismatch!" << std::endl;
        std::cerr << "expected: ";
        for (unsigned char c : expected_hash) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
        std::cerr << std::endl << "received: ";
        for (unsigned char c : received_hash) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
        std::cerr << std::dec << std::endl;
        return false;
    }

    auto peer_id_data = receive_raw_data(20);
    if (peer_id_data.empty()) {
        std::cerr << "failed to read peer id" << std::endl;
        return false;
    }

    peer_.peer_id_ = std::string(peer_id_data.begin(), peer_id_data.end());
    if (peer_.peer_id_ == our_peer_id_) {
        std::cerr << "connected to ourselves, disconnecting" << std::endl;
        return false;
    }
    std::cout << "handshake successful with peer " << peer_.peer_id_.substr(0, 8) << "..." << std::endl;
    return true;
}

std::vector<uint8_t> PeerConnection::create_handshake_message() {
    std::vector<uint8_t> handshake;
    handshake.push_back(19);

    std::string protocol = "BitTorrent protocol";
    handshake.insert(handshake.end(), protocol.begin(), protocol.end());

    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x10);
    handshake.push_back(0x00);
    handshake.push_back(0x01);

    const std::string& info_hash = torrent_->get_info_hash();
    handshake.insert(handshake.end(), info_hash.begin(), info_hash.end());
    handshake.insert(handshake.end(), our_peer_id_.begin(), our_peer_id_.end());
    return handshake;
}

bool PeerConnection::send_message_immediate(const std::vector<uint8_t>& message) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    return send_raw_data(message.data(), message.size());
}

bool PeerConnection::send_interested() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (state_.am_interested) {
        return true;
    }

    auto message = create_message(INTERESTED);
    if (send_message_immediate(message)) {
        state_.am_interested = true;
        std::cout << "sent interested to " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send interested to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_not_interested() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!state_.am_interested) {
        return true;
    }

    auto message = create_message(NOT_INTERESTED);
    if (send_message_immediate(message)) {
        state_.am_interested = false;
        std::cout << "sent not_interested to " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send not_interested to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_choke() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (state_.am_choking) {
        return true;
    }

    auto message = create_message(CHOKE);
    if (send_message_immediate(message)) {
        state_.am_choking = true;
        std::cout << "sent choke to " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send choke to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_unchoke() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!state_.am_choking) {
        return true;
    }

    auto message = create_message(UNCHOKE);
    if (send_message_immediate(message)) {
        state_.am_choking = false;
        std::cout << "sent unchoke to " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send unchoke to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_bitfield() {
    std::cout << "sending bitfield to " << peer_.to_string() << std::endl;

    size_t total_pieces = torrent_->get_piece_hashes().size();
    size_t bytes_needed = (total_pieces + 7) / 8;
    std::vector<uint8_t> bitfield_data(bytes_needed, 0);

    for (size_t piece = 0; piece < total_pieces; ++piece) {
        if (peer_manager_->get_piece_manager()->has_piece(piece)) {
            size_t byte_index = piece / 8;
            size_t bit_index = piece % 8;
            bitfield_data[byte_index] |= (0x80 >> bit_index);
        }
    }

    std::vector<uint8_t> message;
    uint32_t msg_length = 1 + bitfield_data.size();
    message.push_back((msg_length >> 24) & 0xFF);
    message.push_back((msg_length >> 16) & 0xFF);
    message.push_back((msg_length >> 8) & 0xFF);
    message.push_back(msg_length & 0xFF);
    message.push_back(static_cast<uint8_t>(BITFIELD));
    message.insert(message.end(), bitfield_data.begin(), bitfield_data.end());

    if (send_message_immediate(message)) {
        std::cout << "successfully sent bitfield to " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send bitfield to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_request(uint32_t piece, uint32_t offset, uint32_t length) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (state_.peer_choking) {
        std::cout << "cannot request piece " << piece << " - peer is choking us" << std::endl;
        return false;
    }

    if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
        std::cout << "cannot request piece " << piece << " - too many pending requests" << std::endl;
        return false;
    }

    if (!peer_has_piece(piece)) {
        std::cout << "cannot request piece " << piece << " - peer doesn't have it" << std::endl;
        return false;
    }

    auto message = create_request_message(piece, offset, length);
    if (send_message_immediate(message)) {
        pending_requests_.emplace(piece, offset, length);
        std::cout << "requested piece " << piece << " block " << offset
                  << " (" << length << " bytes) from " << peer_.to_string() << std::endl;
        return true;
    } else {
        std::cerr << "failed to send request to " << peer_.to_string() << std::endl;
        return false;
    }
}

bool PeerConnection::send_cancel(uint32_t piece, uint32_t offset, uint32_t length) {
    auto message = create_cancel_message(piece, offset, length);
    if (send_message_immediate(message)) {
        std::cout << "sent cancel for piece " << piece << " block " << offset << std::endl;
        return true;
    } else {
        std::cerr << "failed to send cancel" << std::endl;
        return false;
    }
}

bool PeerConnection::send_have(uint32_t piece_index) {
    auto message = create_have_message(piece_index);
    if (send_message_immediate(message)) {
        std::cout << "sent have for piece " << piece_index << std::endl;
        return true;
    } else {
        std::cerr << "failed to send have for piece " << piece_index << std::endl;
        return false;
    }
}

void PeerConnection::receive_loop() {
    std::cout << "started receive loop for " << peer_.to_string() << std::endl;

    while (running_ && connected_) {
        if (!receive_message()) {
            std::cerr << "failed to receive message from " << peer_.to_string() << std::endl;
            break;
        }
        cleanup_expired_requests();
    }

    std::cout << "receive loop ended for " << peer_.to_string() << std::endl;
}

bool PeerConnection::receive_message() {
    auto length_data = receive_raw_data(4);
    if (length_data.size() != 4) {
        return false;
    }

    uint32_t message_length = (static_cast<uint32_t>(length_data[0]) << 24) |
                             (static_cast<uint32_t>(length_data[1]) << 16) |
                             (static_cast<uint32_t>(length_data[2]) << 8) |
                             static_cast<uint32_t>(length_data[3]);

    if (message_length == 0) {
        return true;
    }

    auto type_data = receive_raw_data(1);
    if (type_data.empty()) {
        return false;
    }

    MessageType msg_type = static_cast<MessageType>(type_data[0]);

    std::vector<uint8_t> payload;
    if (message_length > 1) {
        payload = receive_raw_data(message_length - 1);
        if (payload.size() != message_length - 1) {
            return false;
        }
    }

    switch (msg_type) {
        case CHOKE:
            handle_choke();
            break;
        case UNCHOKE:
            handle_unchoke();
            break;
        case INTERESTED:
            handle_interested();
            break;
        case NOT_INTERESTED:
            handle_not_interested();
            break;
        case HAVE:
            if (payload.size() == 4) {
                uint32_t piece = (static_cast<uint32_t>(payload[0]) << 24) |
                               (static_cast<uint32_t>(payload[1]) << 16) |
                               (static_cast<uint32_t>(payload[2]) << 8) |
                               static_cast<uint32_t>(payload[3]);
                handle_have(piece);
            }
            break;
        case BITFIELD:
            handle_bitfield(payload);
            break;
        case REQUEST:
            if (payload.size() == 12) {
                uint32_t piece = (static_cast<uint32_t>(payload[0]) << 24) |
                               (static_cast<uint32_t>(payload[1]) << 16) |
                               (static_cast<uint32_t>(payload[2]) << 8) |
                               static_cast<uint32_t>(payload[3]);
                uint32_t offset = (static_cast<uint32_t>(payload[4]) << 24) |
                                (static_cast<uint32_t>(payload[5]) << 16) |
                                (static_cast<uint32_t>(payload[6]) << 8) |
                                static_cast<uint32_t>(payload[7]);
                uint32_t length = (static_cast<uint32_t>(payload[8]) << 24) |
                                (static_cast<uint32_t>(payload[9]) << 16) |
                                (static_cast<uint32_t>(payload[10]) << 8) |
                                static_cast<uint32_t>(payload[11]);
                handle_request(piece, offset, length);
            }
            break;
        case PIECE:
            if (payload.size() >= 8) {
                uint32_t piece = (static_cast<uint32_t>(payload[0]) << 24) |
                               (static_cast<uint32_t>(payload[1]) << 16) |
                               (static_cast<uint32_t>(payload[2]) << 8) |
                               static_cast<uint32_t>(payload[3]);
                uint32_t offset = (static_cast<uint32_t>(payload[4]) << 24) |
                                (static_cast<uint32_t>(payload[5]) << 16) |
                                (static_cast<uint32_t>(payload[6]) << 8) |
                                static_cast<uint32_t>(payload[7]);
                std::vector<uint8_t> block_data(payload.begin() + 8, payload.end());
                handle_piece(piece, offset, block_data);
            }
            break;
        case CANCEL:
            if (payload.size() == 12) {
                uint32_t piece = (static_cast<uint32_t>(payload[0]) << 24) |
                               (static_cast<uint32_t>(payload[1]) << 16) |
                               (static_cast<uint32_t>(payload[2]) << 8) |
                               static_cast<uint32_t>(payload[3]);
                uint32_t offset = (static_cast<uint32_t>(payload[4]) << 24) |
                                (static_cast<uint32_t>(payload[5]) << 16) |
                                (static_cast<uint32_t>(payload[6]) << 8) |
                                static_cast<uint32_t>(payload[7]);
                uint32_t length = (static_cast<uint32_t>(payload[8]) << 24) |
                                (static_cast<uint32_t>(payload[9]) << 16) |
                                (static_cast<uint32_t>(payload[10]) << 8) |
                                static_cast<uint32_t>(payload[11]);
                handle_cancel(piece, offset, length);
            }
            break;
        default:
            std::cout << "unknown message type " << static_cast<int>(msg_type)
                      << " from " << peer_.to_string() << std::endl;
            break;
    }
    return true;
}

void PeerConnection::handle_choke() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.peer_choking = true;  // Fixed typo
    std::cout << "peer " << peer_.to_string() << " choked us" << std::endl;
}

void PeerConnection::handle_unchoke() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.peer_choking = false;  // Fixed typo
    std::cout << "peer " << peer_.to_string() << " unchoked us" << std::endl;
    peer_manager_->request_more_work(this);
}

void PeerConnection::handle_interested() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.peer_interested = true;
    std::cout << "peer " << peer_.to_string() << " is interested in us" << std::endl;
}

void PeerConnection::handle_not_interested() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.peer_interested = false;
    std::cout << "peer " << peer_.to_string() << " is not interested in us" << std::endl;
}

void PeerConnection::handle_have(uint32_t piece_index) {
    if (piece_index < peer_bitfield_.size()) {
        peer_bitfield_[piece_index] = true;
        std::cout << "peer " << peer_.to_string() << " has piece " << piece_index << std::endl;
        update_interest_state();
    }
}

void PeerConnection::handle_bitfield(const std::vector<uint8_t>& bitfield_data) {
    std::cout << "received bitfield from " << peer_.to_string()
              << " (" << bitfield_data.size() << " bytes)" << std::endl;

    size_t piece_count = 0;
    for (size_t i = 0; i < bitfield_data.size() && i * 8 < peer_bitfield_.size(); ++i) {
        uint8_t byte = bitfield_data[i];
        for (int bit = 0; bit < 8 && i * 8 + bit < peer_bitfield_.size(); ++bit) {
            if (byte & (0x80 >> bit)) {
                peer_bitfield_[i * 8 + bit] = true;
                piece_count++;
            }
        }
    }

    std::cout << "peer has " << piece_count << " pieces out of "
              << torrent_->get_piece_hashes().size() << " total" << std::endl;

    update_interest_state();
}

void PeerConnection::handle_request(uint32_t piece, uint32_t offset, uint32_t length) {
    std::cout << "received request for piece " << piece << " offset " << offset
              << " length " << length << " from " << peer_.to_string() << std::endl;

    // todo implement upload
}

void PeerConnection::handle_piece(uint32_t piece, uint32_t offset, const std::vector<uint8_t>& data) {
    std::cout << "received piece " << piece << " block " << offset
              << " (" << data.size() << " bytes) from " << peer_.to_string() << std::endl;

    peer_.bytes_downloaded_ += data.size();
    bytes_received_ += data.size();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::queue<PendingRequest> remaining_requests;
        bool found = false;

        while (!pending_requests_.empty()) {
            const auto& req = pending_requests_.front();
            if (!found && req.piece_index == piece && req.block_offset == offset) {
                found = true;
            } else {
                remaining_requests.push(req);
            }
            pending_requests_.pop();
        }
        pending_requests_ = std::move(remaining_requests);
    }

    std::vector<char> char_data(data.begin(), data.end());
    bool success = peer_manager_->get_piece_manager()->add_piece_block(piece, offset, char_data);
    if (success) {
        std::cout << "block added to piece manager" << std::endl;
        if (pending_requests_.size() < MAX_PENDING_REQUESTS / 2) {
            std::cout << "requesting more work (capacity available)" << std::endl;
            peer_manager_->request_more_work(this);
        }
    } else {
        std::cout << "failed to add piece block" << std::endl;
    }
}

void PeerConnection::handle_cancel(uint32_t piece, uint32_t offset, uint32_t length) {
    std::cout << "received cancel for piece " << piece << " offset " << offset
              << " length " << length << " from " << peer_.to_string() << std::endl;

    // todo implement uploading, download only currently
}

void PeerConnection::update_interest_state() {
    bool should_be_interested_now = should_be_interested();

    std::cout << "updating interest state for " << peer_.to_string()
              << " - should be interested: " << (should_be_interested_now ? "YES" : "NO")
              << " - currently told peer: " << (state_.am_interested ? "INTERESTED" : "NOT_INTERESTED") << std::endl;

    if (should_be_interested_now && !state_.am_interested) {
        if (!send_interested()) {
            std::cerr << "failed to send interested - connection may be broken" << std::endl;
        }
    } else if (!should_be_interested_now && state_.am_interested) {
        if (!send_not_interested()) {
            std::cerr << "failed to send not_interested - connection may be broken" << std::endl;
        }
    }
}

bool PeerConnection::should_be_interested() const {
    size_t total_pieces = torrent_->get_piece_hashes().size();
    for (size_t piece = 0; piece < total_pieces && piece < peer_bitfield_.size(); ++piece) {
        if (peer_bitfield_[piece] && !peer_manager_->get_piece_manager()->has_piece(piece)) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> PeerConnection::create_message(MessageType type) {
    std::vector<uint8_t> message;
    message.push_back(0);
    message.push_back(0);
    message.push_back(0);
    message.push_back(1);
    message.push_back(static_cast<uint8_t>(type));
    return message;
}

std::vector<uint8_t> PeerConnection::create_have_message(uint32_t piece_index) {
    std::vector<uint8_t> message;

    message.push_back(0);
    message.push_back(0);
    message.push_back(0);
    message.push_back(5);


    message.push_back(static_cast<uint8_t>(HAVE));

    message.push_back((piece_index >> 24) & 0xFF);
    message.push_back((piece_index >> 16) & 0xFF);
    message.push_back((piece_index >> 8) & 0xFF);
    message.push_back(piece_index & 0xFF);

    return message;
}

std::vector<uint8_t> PeerConnection::create_request_message(uint32_t piece,
                                                            uint32_t offset,
                                                            uint32_t length) {
  std::vector<uint8_t> message;

  message.push_back(0);
  message.push_back(0);
  message.push_back(0);
  message.push_back(13);

  message.push_back(static_cast<uint8_t>(REQUEST));

  message.push_back((piece >> 24) & 0xFF);
  message.push_back((piece >> 16) & 0xFF);
  message.push_back((piece >> 8) & 0xFF);
  message.push_back(piece & 0xFF);

  message.push_back((offset >> 24) & 0xFF);
  message.push_back((offset >> 16) & 0xFF);
  message.push_back((offset >> 8) & 0xFF);
  message.push_back(offset & 0xFF);

  message.push_back((length >> 24) & 0xFF);
  message.push_back((length >> 16) & 0xFF);
  message.push_back((length >> 8) & 0xFF);
  message.push_back(length & 0xFF);

  return message;
}

std::vector<uint8_t>
PeerConnection::create_piece_message(uint32_t piece, uint32_t offset,
                                     const std::vector<uint8_t> &data) {
  std::vector<uint8_t> message;

  uint32_t msg_length = 1 + 4 + 4 + data.size();
  message.push_back((msg_length >> 24) & 0xFF);
  message.push_back((msg_length >> 16) & 0xFF);
  message.push_back((msg_length >> 8) & 0xFF);
  message.push_back(msg_length & 0xFF);
  message.push_back(static_cast<uint8_t>(PIECE));


  message.push_back((piece >> 24) & 0xFF);
  message.push_back((piece >> 16) & 0xFF);
  message.push_back((piece >> 8) & 0xFF);
  message.push_back(piece & 0xFF);

  message.push_back((offset >> 24) & 0xFF);
  message.push_back((offset >> 16) & 0xFF);
  message.push_back((offset >> 8) & 0xFF);
  message.push_back(offset & 0xFF);

  message.insert(message.end(), data.begin(), data.end());

  return message;
}

std::vector<uint8_t> PeerConnection::create_cancel_message(uint32_t piece, uint32_t offset, uint32_t length) {
    std::vector<uint8_t> message;

    message.push_back(0);
    message.push_back(0);
    message.push_back(0);
    message.push_back(13);

    message.push_back(static_cast<uint8_t>(CANCEL));

    message.push_back((piece >> 24) & 0xFF);
    message.push_back((piece >> 16) & 0xFF);
    message.push_back((piece >> 8) & 0xFF);
    message.push_back(piece & 0xFF);

    message.push_back((offset >> 24) & 0xFF);
    message.push_back((offset >> 16) & 0xFF);
    message.push_back((offset >> 8) & 0xFF);
    message.push_back(offset & 0xFF);

    message.push_back((length >> 24) & 0xFF);
    message.push_back((length >> 16) & 0xFF);
    message.push_back((length >> 8) & 0xFF);
    message.push_back(length & 0xFF);

    return message;
}

void PeerConnection::cleanup_expired_requests() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto now = std::chrono::steady_clock::now();

    while (!pending_requests_.empty()) {
        const auto& req = pending_requests_.front();
        if (now - req.request_time > REQUEST_TIMEOUT) {
            std::cout << "request timeout for piece " << req.piece_index
                      << " block " << req.block_offset << " from " << peer_.to_string() << std::endl;
            pending_requests_.pop();
        } else {
            break;
        }
    }
}

std::vector<uint8_t> PeerConnection::receive_raw_data(size_t length) {
    std::vector<uint8_t> data(length);
    size_t received = 0;
    while (received < length && running_ && connected_) {
        ssize_t result = recv(socket_fd_, data.data() + received, length - received, 0);
        if (result <= 0) {
            return {};
        }
        received += result;
        bytes_received_ += result;
    }
    return data;
}

bool PeerConnection::send_raw_data(const uint8_t* data, size_t length) {
    if (!connected_ || socket_fd_ < 0) {
        return false;
    }

    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(socket_fd_, data + sent, length - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            return false;
        }
        sent += result;
        bytes_sent_ += result;
    }
    return true;
}

bool PeerConnection::create_socket() {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    return socket_fd_ >= 0;
}

void PeerConnection::close_socket() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

PeerConnection::PeerState PeerConnection::get_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

bool PeerConnection::peer_has_piece(uint32_t piece_index) const {
    return piece_index < peer_bitfield_.size() && peer_bitfield_[piece_index];
}

size_t PeerConnection::get_available_piece_count() const {
    return peer_bitfield_.count();
}

size_t PeerConnection::get_pending_request_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pending_requests_.size();
}

double PeerConnection::get_download_rate() const {
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - connection_start_).count();
    return duration > 0 ? static_cast<double>(bytes_received_) / duration : 0.0;
}

std::chrono::seconds PeerConnection::get_connection_duration() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - connection_start_);
}