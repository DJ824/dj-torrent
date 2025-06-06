#include "tracker.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>
#include <utility>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

SSL_CTX* TrackerClient::ssl_ctx_ = nullptr;
bool TrackerClient::ssl_initialized_ = false;

TrackerClient::TrackerClient(TorrentFile* torrent, uint16_t port)
    : torrent_(torrent), listen_port_(port) {
    peer_id_ = generate_peer_id();
    calculate_stats();

    std::cout << "TrackerClient initialized with peer ID: "
        << peer_id_.substr(0, 8) << "..." << std::endl;
}

TrackerClient::~TrackerClient() {
    stop_announcing();
    if (ssl_initialized_) {
        EVP_cleanup();
        ERR_free_strings();
    }
    cleanup_ssl();
}

TrackerClient::TrackerResponse TrackerClient::announce(const std::string& event) {
    std::cout << "Contacting tracker: " << torrent_->get_announce_url() << std::endl;
    std::string request_url = build_announce_url(event);
    std::cout << "Request URL: " << request_url << std::endl;
    std::string response_data = make_http_request(request_url);
    if (response_data.empty()) {
        TrackerResponse response;
        response.failure_reason = "Failed to contact tracker";
        return response;
    }
    return parse_tracker_response(response_data);
}

void TrackerClient::update_stats(size_t uploaded, size_t downloaded) {
    uploaded_ = uploaded;
    downloaded_ = downloaded;
    calculate_stats();
}

const std::string& TrackerClient::get_peer_id() const {
    return peer_id_;
}

uint16_t TrackerClient::get_listen_port() const {
    return listen_port_;
}

void TrackerClient::start_announcing(std::function<void(std::vector<Peer>)> callback) {
    if (running_.exchange(true)) {
        std::cout << "tracker client already running" << std::endl;
        return;
    }

    peer_callback_ = std::move(callback);
    std::cout << "starting tracker announces" << std::endl;
    announce_thread_ = std::thread(&TrackerClient::announce_loop, this);
}

void TrackerClient::stop_announcing() {
    if (running_.exchange(false)) {
        return;
    }
    std::cout << "stopping tracker announces" << std::endl;
    if (announce_thread_.joinable()) {
        announce_thread_.join();
    }
    std::cout << "tracker client stopped" << std::endl;
}

bool TrackerClient::is_running() const {
    return running_;
}

void TrackerClient::announce_loop() {
    std::cout << "tracker announce loop started" << std::endl;
    std::cout << "sending initial 'started' announce" << std::endl;
    auto response = announce("started");
    if (!response.failure_reason.empty()) {
        std::cerr << "failed to start announcing: " << response.failure_reason << std::endl;
        running_ = false;
        return;
    }

    if (peer_callback_ && !response.peers.empty()) {
        std::cout << "delivering " << response.peers.size() << " initial peers" << std::endl;
        peer_callback_(response.peers);
    }

    current_interval_ = std::chrono::seconds(response.interval);
    std::cout << "tracker interval set to " << current_interval_.count() << " seconds" << std::endl;

    while (running_) {
        std::cout << "sleeping for " << current_interval_.count() << " seconds" << std::endl;
        auto remaining = current_interval_;
        while (remaining > std::chrono::seconds(0) && running_) {
            auto sleep_time = std::min(remaining, std::chrono::seconds(5));
            std::this_thread::sleep_for(sleep_time);
            remaining -= sleep_time;
        }
        if (!running_) {
            break;
        }
        std::cout << "sending interval announce" << std::endl;
        auto interval_response = announce();
        if (!interval_response.failure_reason.empty()) {
            std::cout << "interval response failed: " << interval_response.failure_reason << std::endl;
            continue;
        }

        if (peer_callback_ && !interval_response.peers.empty()) {
            std::cout << "delivering " << interval_response.peers.size() << " additional peers" << std::endl;
            peer_callback_(interval_response.peers);
        }

        auto new_interval = std::chrono::seconds(interval_response.interval);
        if (new_interval != current_interval_) {
            std::cout << "tracker interval updated from " << current_interval_.count() << " seconds" <<
                " to " << new_interval.count() << " seconds" << std::endl;
            current_interval_ = new_interval;
        }
    }

    std::cout << "sending final 'stopped' announce" << std::endl;
    auto stopped_response = announce("stopped");
    if (!stopped_response.failure_reason.empty()) {
        std::cerr << "failed to send stopped announce: " << stopped_response.failure_reason << std::endl;
    }
    std::cout << "tracker announce loop finished" << std::endl;
}



std::string TrackerClient::generate_peer_id() {
    std::string peer_id = "-DJ0001-";
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    for (int i = 0; i < 12; ++i) {
        peer_id += charset[dis(gen)];
    }
    return peer_id;
}

void TrackerClient::calculate_stats() {
    left_ = 0;
    for (const auto& file : torrent_->get_files()) {
        left_ += file.length_;
    }
    left_ -= downloaded_;
}


std::string TrackerClient::build_announce_url(const std::string& event) {
    std::string url = torrent_->get_announce_url();

    url += "?info_hash=" + url_encode(torrent_->get_info_hash());
    url += "&peer_id=" + url_encode(peer_id_);
    url += "&port=" + std::to_string(listen_port_);
    url += "&uploaded=" + std::to_string(uploaded_);
    url += "&downloaded=" + std::to_string(downloaded_);
    url += "&left=" + std::to_string(left_);
    url += "&compact=1";

    if (!event.empty()) {
        url += "&event=" + event;
    }
    return url;
}

std::string TrackerClient::url_encode(const std::string& str) {
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase;

    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        }
        else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return encoded.str();
}

std::tuple<std::string, int, std::string> TrackerClient::parse_url(const std::string& url) {
    bool is_https = false;
    size_t scheme_end = 0;

    if (url.substr(0, 8) == "https://") {
        is_https = true;
        scheme_end = 8;
    } else if (url.substr(0, 7) == "http://") {
        is_https = false;
        scheme_end = 7;
    } else {
        return {"", 0, ""};
    }

    size_t host_start = scheme_end;
    size_t path_start = url.find('/', host_start);
    std::string host_port = url.substr(host_start, path_start - host_start);
    std::string path = (path_start != std::string::npos) ? url.substr(path_start) : "/";
    size_t colon_pos = host_port.find(':');
    std::string host;
    int port;

    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        host = host_port;
        port = is_https ? 443 : 80;
    }

    return {host, port, path};
}

std::string TrackerClient::make_http_request(const std::string& url) {
    auto [host, port, path] = parse_url(url);

    if (host.empty()) {
        std::cerr << "Failed to parse URL: " << url << std::endl;
        return "";
    }

    std::cout << "Connecting to " << host << ":" << port << path << std::endl;

    bool use_ssl = (port == 443 || url.substr(0, 8) == "https://");

    if (use_ssl) {
        initialize_ssl();
        return make_https_request(host, port, path);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return "";
    }

    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        std::cerr << "Failed to resolve hostname: " << host << std::endl;
        close(sock);
        return "";
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to " << host << ":" << port << std::endl;
        close(sock);
        return "";
    }

    std::string request = build_http_request(host, path);

    if (send(sock, request.c_str(), request.length(), 0) < 0) {
        std::cerr << "Failed to send HTTP request" << std::endl;
        close(sock);
        return "";
    }
    std::string response;
    char buffer[4096];
    ssize_t bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }
    close(sock);
    return extract_http_body(response);
}

std::string TrackerClient::make_https_request(const std::string& host, int port, const std::string& path) {
    std::cout << "Making HTTPS request to " << host << ":" << port << path << std::endl;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create socket for HTTPS" << std::endl;
        return "";
    }

    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        std::cerr << "Failed to resolve hostname: " << host << std::endl;
        close(sock);
        return "";
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to " << host << ":" << port << std::endl;
        close(sock);
        return "";
    }

    SSL* ssl = SSL_new(ssl_ctx_);
    if (!ssl) {
        std::cerr << "Failed to create SSL structure" << std::endl;
        close(sock);
        return "";
    }

    SSL_set_fd(ssl, sock);

    int ssl_connect_result = SSL_connect(ssl);
    if (ssl_connect_result != 1) {
        std::cerr << "SSL handshake failed with error: " << SSL_get_error(ssl, ssl_connect_result) << std::endl;
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(sock);
        return "";
    }

    std::cout << "SSL handshake successful" << std::endl;

    std::string request = build_http_request(host, path);
    int ssl_write_result = SSL_write(ssl, request.c_str(), request.length());
    if (ssl_write_result <= 0) {
        std::cerr << "Failed to send HTTPS request" << std::endl;
        SSL_free(ssl);
        close(sock);
        return "";
    }

    std::string response;
    char buffer[4096];
    int bytes_received;
    while ((bytes_received = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }

    SSL_free(ssl);
    close(sock);
    std::cout << "HTTPS request completed, response size: " << response.size() << " bytes" << std::endl;
    return extract_http_body(response);
}

std::string TrackerClient::build_http_request(const std::string& host, const std::string& path) {
    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "User-Agent: DJ-Torrent/0.1\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";
    return request.str();
}

std::string TrackerClient::extract_http_body(const std::string& http_response) {
    size_t body_start = http_response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        std::cerr << "Invalid HTTP response format" << std::endl;
        return "";
    }
    body_start += 4;
    return http_response.substr(body_start);
}

void TrackerClient::initialize_ssl() {
    if (ssl_initialized_) {
        return;
    }
    std::cout << "initializing SSL" << std::endl;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
        std::cerr << "failed to create ssl context" << std::endl;
        return;
    }
    SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    ssl_initialized_ = true;
    std::cout << "ssl initialized" << std::endl;
}

void TrackerClient::cleanup_ssl() {
    if (ssl_initialized_ && ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        ssl_initialized_ = false;
    }
}

TrackerClient::TrackerResponse TrackerClient::parse_tracker_response(const std::string& response_data) {
    TrackerResponse response;

    auto root = BencodeParser::parse(response_data);

    if (!root || !root->is_dict()) {
        response.failure_reason = "Invalid tracker response format";
        return response;
    }

    const auto& dict = root->as_dict();
    auto failure_it = dict.find("failure reason");
    if (failure_it != dict.end() && failure_it->second->is_string()) {
        response.failure_reason = failure_it->second->as_string();
        return response;
    }

    auto interval_it = dict.find("interval");
    if (interval_it != dict.end() && interval_it->second->is_integer()) {
        response.interval = static_cast<int32_t>(interval_it->second->as_integer());
    }

    auto complete_it = dict.find("complete");
    if (complete_it != dict.end() && complete_it->second->is_integer()) {
        response.complete = static_cast<int32_t>(complete_it->second->as_integer());
    }

    auto incomplete_it = dict.find("incomplete");
    if (incomplete_it != dict.end() && incomplete_it->second->is_integer()) {
        response.incomplete = static_cast<int32_t>(incomplete_it->second->as_integer());
    }

    auto peers_it = dict.find("peers");
    if (peers_it != dict.end()) {
        if (peers_it->second->is_string()) {
            parse_peers_compact(peers_it->second->as_string(), response.peers);
        }
        else if (peers_it->second->is_list()) {
            parse_peers_list(peers_it->second->as_list(), response.peers);
        }
    }

    std::cout << "Tracker response: " << response.peers.size() << " peers, "
        << response.complete << " seeders, " << response.incomplete << " leechers" << std::endl;

    return response;
}

void TrackerClient::parse_peers_compact(const std::string& peers_data, std::vector<Peer>& peers) {
    for (size_t i = 0; i + 6 <= peers_data.length(); i += 6) {
        uint8_t ip_bytes[4];
        for (int j = 0; j < 4; ++j) {
            ip_bytes[j] = static_cast<uint8_t>(peers_data[i + j]);
        }

        std::string ip = std::to_string(ip_bytes[0]) + "." +
            std::to_string(ip_bytes[1]) + "." +
            std::to_string(ip_bytes[2]) + "." +
            std::to_string(ip_bytes[3]);

        uint16_t port = (static_cast<uint8_t>(peers_data[i + 4]) << 8) |
            static_cast<uint8_t>(peers_data[i + 5]);
        peers.emplace_back(ip, port);
    }
}

void TrackerClient::parse_peers_list(const BencodeList& peers_list, std::vector<Peer>& peers) {
    for (const auto& peer_value : peers_list) {
        if (!peer_value->is_dict()) {
            continue;
        }
        const auto& peer_dict = peer_value->as_dict();
        auto ip_it = peer_dict.find("ip");
        auto port_it = peer_dict.find("port");
        if (ip_it != peer_dict.end() && ip_it->second->is_string() &&
            port_it != peer_dict.end() && port_it->second->is_integer()) {
            std::string ip = ip_it->second->as_string();
            uint16_t port = static_cast<uint16_t>(port_it->second->as_integer());
            peers.emplace_back(ip, port);
        }
    }
}

//  Hash v1: 611f70899d4e1d6a9c39cfc925f103dfef630328