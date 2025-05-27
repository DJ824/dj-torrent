#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <random>
#include <memory>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "torrent_file.h"
#include "bencode_parser.h"

class TrackerClient {
public:
    struct Peer {
        std::string ip;
        uint16_t port;

        Peer(const std::string& ip_addr, uint16_t port_num)
            : ip(ip_addr), port(port_num) {
        }
    };

    struct TrackerResponse {
        std::vector<Peer> peers;
        int32_t interval = 1800;
        int32_t min_interval = 900;
        int32_t complete = 0;
        int32_t incomplete = 0;
        std::string failure_reason;
    };

private:
    TorrentFile* torrent_;
    std::string peer_id_;
    uint16_t listen_port_;

    size_t uploaded_ = 0;
    size_t downloaded_ = 0;
    size_t left_ = 0;

public:
    explicit TrackerClient(TorrentFile* torrent, uint16_t port = 6881)
        : torrent_(torrent), listen_port_(port) {
        peer_id_ = generate_peer_id();
        calculate_stats();

        std::cout << "TrackerClient initialized with peer ID: "
            << peer_id_.substr(0, 8) << "..." << std::endl;
    }

    TrackerResponse announce(const std::string& event = "") {
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

    void update_stats(size_t uploaded, size_t downloaded) {
        uploaded_ = uploaded;
        downloaded_ = downloaded;
        calculate_stats();
    }

private:
    std::string generate_peer_id() {
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

    void calculate_stats() {
        left_ = 0;
        for (const auto& file : torrent_->get_files()) {
            left_ += file.length_;
        }
        left_ -= downloaded_;
    }

    std::string build_announce_url(const std::string& event) {
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

    std::string url_encode(const std::string& str) {
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

    std::tuple<std::string, int, std::string> parse_url(const std::string& url) {
        if (url.substr(0, 7) != "http://") {
            return {"", 0, ""};
        }

        size_t host_start = 7;
        size_t path_start = url.find('/', host_start);
        std::string host_port = url.substr(host_start, path_start - host_start);
        std::string path = (path_start != std::string::npos) ? url.substr(path_start) : "/";
        size_t colon_pos = host_port.find(':');
        std::string host;
        int port = 80;

        if (colon_pos != std::string::npos) {
            host = host_port.substr(0, colon_pos);
            port = std::stoi(host_port.substr(colon_pos + 1));
        }
        else {
            host = host_port;
        }

        return {host, port, path};
    }

    std::string make_http_request(const std::string& url) {
        auto [host, port, path] = parse_url(url);

        if (host.empty()) {
            std::cerr << "Failed to parse URL: " << url << std::endl;
            return "";
        }

        std::cout << "Connecting to " << host << ":" << port << path << std::endl;

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

    std::string build_http_request(const std::string& host, const std::string& path) {
        std::ostringstream request;
        request << "GET " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << "\r\n";
        request << "User-Agent: DJ-Torrent/0.1\r\n";
        request << "Connection: close\r\n";
        request << "\r\n";
        return request.str();
    }

    std::string extract_http_body(const std::string& http_response) {
        size_t body_start = http_response.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            std::cerr << "Invalid HTTP response format" << std::endl;
            return "";
        }
        body_start += 4;
        return http_response.substr(body_start);
    }

    TrackerResponse parse_tracker_response(const std::string& response_data) {
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

    void parse_peers_compact(const std::string& peers_data, std::vector<Peer>& peers) {
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

    void parse_peers_list(const BencodeList& peers_list, std::vector<Peer>& peers) {
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

public:
    const std::string& get_peer_id() const { return peer_id_; }
    uint16_t get_listen_port() const { return listen_port_; }
};
