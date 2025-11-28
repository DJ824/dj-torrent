#include "tracker_client.h"

#include "bencode.h"
#include "http_client.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <variant>

static std::string url_encode(std::string_view data) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : data) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

static std::string url_encode_bytes(const uint8_t* data, size_t len) {
    return url_encode(std::string_view(reinterpret_cast<const char*>(data), len));
}

static std::string build_query(const TorrentFile& torrent,
                        const std::string& peer_id,
                        uint16_t port,
                        int64_t downloaded,
                        int64_t uploaded,
                        int64_t left,
                        const std::string& event) {
    std::ostringstream oss;
    oss << "info_hash=" << url_encode_bytes(torrent.info_hash.data(), torrent.info_hash.size());
    oss << "&peer_id=" << url_encode(peer_id);
    oss << "&port=" << port;
    oss << "&uploaded=" << uploaded;
    oss << "&downloaded=" << downloaded;
    oss << "&left=" << left;
    oss << "&compact=1";
    if (!event.empty()) {
        oss << "&event=" << url_encode(event);
    }
    return oss.str();
}

static std::vector<PeerEndpoint> parse_compact_peers(const std::string& peers_blob) {
    if (peers_blob.size() % 6 != 0) {
        throw std::runtime_error("Invalid peers compact string length");
    }
    std::vector<PeerEndpoint> peers;
    peers.reserve(peers_blob.size() / 6);
    for (size_t i = 0; i < peers_blob.size(); i += 6) {
        PeerEndpoint p{};
        char ip_str[INET_ADDRSTRLEN] = {0};
        const unsigned char* data = reinterpret_cast<const unsigned char*>(peers_blob.data() + i);
        inet_ntop(AF_INET, data, ip_str, sizeof(ip_str));
        p.ip = ip_str;
        uint16_t port = static_cast<uint16_t>(data[4] << 8 | data[5]);
        p.port = port;
        peers.push_back(std::move(p));
    }
    return peers;
}


TrackerClient::TrackerClient(std::string peer_id, uint16_t port)
    : peer_id_(std::move(peer_id)), port_(port) {}

AnnounceResponse TrackerClient::announce(const std::string& announce_url,
                                         const TorrentFile& torrent,
                                         int64_t downloaded,
                                         int64_t uploaded,
                                         std::string event) {
    HttpUrl url = parse_http_url(announce_url);
    std::string query = build_query(torrent, peer_id_, port_, downloaded, uploaded,
                                    std::max<int64_t>(torrent.total_length() - downloaded, 0),
                                    event);

    std::string path = url.path;
    if (path.find('?') == std::string::npos) {
        path += "?";
    } else {
        path += "&";
    }
    path += query;

    HttpResponse http_resp = http_get(url, path);
    if (http_resp.status_code != 200) {
        throw std::runtime_error("Tracker returned non-200 status: " + http_resp.status_line);
    }

    bencode::Parser parser(http_resp.body);
    bencode::Value root_v = parser.parse();
    const auto& dict = bencode::as_dict(root_v);
    if (const auto* failure = bencode::find_field(dict, "failure reason")) {
        throw std::runtime_error("Tracker failure: " + bencode::as_string(*failure));
    }

    AnnounceResponse resp;
    resp.interval = static_cast<int>(bencode::as_int(bencode::require_field(dict, "interval")));
    if (const auto* c = bencode::find_field(dict, "complete")) {
        resp.complete = bencode::as_int(*c);
    }
    if (const auto* inc = bencode::find_field(dict, "incomplete")) {
        resp.incomplete = bencode::as_int(*inc);
    }

    const bencode::Value& peers_v = bencode::require_field(dict, "peers");
    if (std::holds_alternative<std::string>(peers_v.data)) {
        resp.peers = parse_compact_peers(std::get<std::string>(peers_v.data));
    } else {
        const auto& peer_list = bencode::as_list(peers_v);
        for (const auto& v : peer_list) {
            const auto& d = bencode::as_dict(v);
            PeerEndpoint p;
            p.ip = bencode::as_string(bencode::require_field(d, "ip"));
            p.port = static_cast<uint16_t>(bencode::as_int(bencode::require_field(d, "port")));
            resp.peers.push_back(std::move(p));
        }
    }

    return resp;
}

std::string generate_peer_id(std::string prefix) {
    if (prefix.size() > 20) prefix = prefix.substr(0, 20);
    std::string id = prefix;
    std::mt19937 rng(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dist(0, 9);
    while (id.size() < 20) {
        id.push_back(static_cast<char>('0' + dist(rng)));
    }
    if (id.size() > 20) id.resize(20);
    return id;
}
