#include <iostream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
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

PeerConnection::~PeerConnection() { disconnect(); }

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
        std::cerr << "invalid ip address " << peer_.ip_ < std::endl;
        close_socket();
        peer_.mark_connection_failure();
        return false;
    }

    if (::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
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
    send_thread_ = std::thread(&PeerConnection::send_loop, this);
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
    if (send_thread_.joinable()) {
        send_thread_.join();
    }

    std::cout << "disconnected from " << peer_.to_string() << std::endl;

}

bool PeerConnection::perform_handshake() {
    if (!send_handshake()) {
        return false;
    }

    if (!receive_handshake) {
        return false;
    }

    handshake_complete_ = true;
    peer_.state = Peer::HANDSHAKED;
    std::cout << "handshake complete with " << peer_.to_string() << std::endl;
    return true;
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

bool PeerConnection::send_handshake() {
    auto handshake = create_handshake_message();
    return send_raw_data(handshake.data(), handshake.size());
}


bool PeerConnection::receive_handshake() {
    auto length_data = receive_raw_data(1);
    if (length_data.empty() || length_data[0] != 19) {
        std::cerr << "invalid handshake protocol length, expected 19";
        return false;
    }

    auto protocol_data = receive_raw_data(19);
    if (protocol_data.empty()) {
        std::cerr << "failed to read protocol string" << std::endl;
        return false;
    }

    std::string protocol_string(protocol_data.begin(), protocol_data.end());
    if (protocol_string != "BitTorrent protocol") {
        std::cerr << "invalid protocol string, expected 'BitTorrent Protocol', got " << protocol_string << std::endl;
        return false;
    }

    auto reserved = receive_raw_data(8);
    if (reserved.empty()) {
        std::cerr << "failed to read reserved bytes" << std::endl;
        return false;
    }

    if (reserved[5] & 0x10) {
        std::cout << "peer supports fast extension" << std::endl;
    }

    if (reserved[7] & 0x10) {
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
    std::cout << "handshake successful with peer " << peer_.peer_id_.substr(0, 8) << std::endl;
    return true;
}

std::vector<uint8_t> PeerConnection::create_handshake_message() {
    std::vector<uint8_t> handshake;
    handshake.push_back(19);
    std::string protocol = "BitTorrent Protocol";
    handshake.insert(handshake.end(), protocol.begin(), protocol.end());
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x00);
    handshake.push_back(0x10);
    handshake.push_back(0x00);
    handshake.push_back(0x01);

    const std::string& info_hash = torrent_get_info_hash();
    handshake.insert(handshake.end(), info_hash.begin(), info_hash.end());
    handshake.insert(handshake.end(), our_peer_id_.begin(), our_peer_id_.end());
    return handshake;
}

bool PeerConnection::send_interested() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_.am_interested) {
        return true;
    }
    auto message = create_message(INTERESTED);
    if (send_message(message)) {
        state_.am_interested = true;
        std::cout << "sent INTERESTED to " << peer_.to_string() << std::endl;
        return true;
    }
    return false;
}

bool PeerConnection::send_request(uint32_t piece, uint32_t offset, uint32_t length) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_.peer_chocking) {
        std::cout << "peer is choking us" << std::endl;
        return false;
    }

    if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
        std::cout << "too many pending requests" << std::endl;
        return false;
    }

    if (!has_piece(piece)) {
        std::cout << "peer does not have piece " << std::endl;
        return false;
    }

    auto message = create_request_message(piece, offset, length);
    if (send_message(message)) {
        pending_requests.emplace(piece, offset, length);
        std::cout << "requested piece " << piece << " block " << offset << " bytes " << length << std::endl;
        return true;
    }
    return false;
}

void PeerConnection::receive_loop() {
    std::cout << "started receive loop for " << peer_.to_string() << std::endl;
    while (running_ && connected_) {
        if (!receive_message()) {
            std::cerr << "failed to receive message from " << peer_.to_string() << std::endl;
            break;
        }
    }
}









