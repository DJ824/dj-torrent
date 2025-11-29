#include "tracker_client.h"

#include "bencode.h"
#include "http_client.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
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

static std::vector<PeerEndpoint> parse_compact_peers_bytes(const char* raw, std::size_t len) {
    if (len % 6 != 0) {
        throw std::runtime_error("Invalid peers compact string length");
    }
    std::vector<PeerEndpoint> peers;
    peers.reserve(len / 6);
    for (std::size_t i = 0; i < len; i += 6) {
        PeerEndpoint p{};
        char ip_str[INET_ADDRSTRLEN] = {0};
        const unsigned char* data = reinterpret_cast<const unsigned char*>(raw + i);
        if (!inet_ntop(AF_INET, data, ip_str, sizeof(ip_str))) {
            throw std::runtime_error("failed to parse IPv4 in tracker response");
        }
        p.ip = ip_str;
        p.port = static_cast<uint16_t>(data[4] << 8 | data[5]);
        peers.push_back(std::move(p));
    }
    return peers;
}

static std::vector<PeerEndpoint> parse_compact_peers(const std::string& peers_blob) {
    return parse_compact_peers_bytes(peers_blob.data(), peers_blob.size());
}

static std::string lowercase_copy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static bool is_udp_tracker_url(const std::string& url) {
    std::string lower = lowercase_copy(url);
    return lower.rfind("udp://", 0) == 0;
}

struct ParsedUdpUrl {
    std::string host;
    std::string port;
};

static ParsedUdpUrl parse_udp_tracker_url(const std::string& url) {
    if (!is_udp_tracker_url(url)) {
        throw std::runtime_error("not a UDP tracker url");
    }
    std::string rest = url.substr(6);
    auto slash_pos = rest.find('/');
    if (slash_pos != std::string::npos) {
        rest = rest.substr(0, slash_pos);
    }
    if (rest.empty()) {
        throw std::runtime_error("invalid UDP tracker url: missing host");
    }

    std::string host;
    std::string port = "80";
    if (rest.front() == '[') {
        auto closing = rest.find(']');
        if (closing == std::string::npos) {
            throw std::runtime_error("invalid UDP tracker IPv6 host");
        }
        host = rest.substr(1, closing - 1);
        if (closing + 1 < rest.size() && rest[closing + 1] == ':') {
            port = rest.substr(closing + 2);
        }
    } else {
        auto colon = rest.find(':');
        if (colon != std::string::npos) {
            host = rest.substr(0, colon);
            port = rest.substr(colon + 1);
        } else {
            host = rest;
        }
    }

    if (host.empty()) {
        throw std::runtime_error("invalid UDP tracker host");
    }
    if (port.empty()) {
        port = "80";
    }
    return ParsedUdpUrl{host, port};
}

static uint32_t random_u32() {
    static thread_local std::mt19937 rng([] {
        std::random_device rd;
        return rd();
    }());
    static thread_local std::uniform_int_distribution<uint32_t> dist;
    return dist(rng);
}

static uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

static uint64_t read_be64(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}

static bool wait_for_readable(int sock, int timeout_ms) {
    struct pollfd pfd {
        sock, POLLIN, 0
    };
    while (true) {
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            return (pfd.revents & POLLIN) != 0;
        }
        if (rc == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

static ssize_t recv_with_timeout(int sock, uint8_t* buf, size_t buf_sz, int timeout_ms) {
    if (!wait_for_readable(sock, timeout_ms)) {
        errno = EAGAIN;
        return -1;
    }
    while (true) {
        ssize_t n = ::recv(sock, buf, buf_sz, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

static void send_all(int sock, const uint8_t* data, size_t len) {
    while (true) {
        ssize_t n = ::send(sock, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("UDP tracker send failed: ") +
                                     std::strerror(errno));
        }
        if (static_cast<size_t>(n) != len) {
            throw std::runtime_error("short UDP tracker send");
        }
        return;
    }
}

static uint32_t udp_event_code(const std::string& event) {
    if (event == "completed") {
        return 1;
    }
    if (event == "started") {
        return 2;
    }
    if (event == "stopped") {
        return 3;
    }
    return 0;
}

static uint64_t udp_tracker_connect(int sock) {
    constexpr uint64_t kProtocolId = 0x41727101980ULL;
    std::array<uint8_t, 16> req{};
    size_t offset = 0;
    auto put64 = [&](uint64_t v) {
        for (int i = 7; i >= 0; --i) {
            req[offset++] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
        }
    };
    auto put32 = [&](uint32_t v) {
        req[offset++] = static_cast<uint8_t>((v >> 24) & 0xFF);
        req[offset++] = static_cast<uint8_t>((v >> 16) & 0xFF);
        req[offset++] = static_cast<uint8_t>((v >> 8) & 0xFF);
        req[offset++] = static_cast<uint8_t>(v & 0xFF);
    };

    uint32_t transaction_id = random_u32();
    put64(kProtocolId);
    put32(0);
    put32(transaction_id);

    std::array<uint8_t, 16> resp{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        send_all(sock, req.data(), offset);
        int timeout_ms = 500 * (1 << attempt);
        ssize_t n = recv_with_timeout(sock, resp.data(), resp.size(), timeout_ms);
        if (n != 16) {
            continue;
        }
        uint32_t action = read_be32(resp.data());
        uint32_t resp_tx = read_be32(resp.data() + 4);
        if (action != 0 || resp_tx != transaction_id) {
            continue;
        }
        return read_be64(resp.data() + 8);
    }
    throw std::runtime_error("UDP tracker connect timed out");
}

static AnnounceResponse udp_tracker_announce(int sock,
                                             uint64_t connection_id,
                                             const TorrentFile& torrent,
                                             const std::string& peer_id,
                                             uint16_t listen_port,
                                             int64_t downloaded,
                                             int64_t uploaded,
                                             const std::string& event) {
    std::array<uint8_t, 128> req{};
    size_t offset = 0;
    auto put64 = [&](uint64_t v) {
        for (int i = 7; i >= 0; --i) {
            req[offset++] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
        }
    };
    auto put32 = [&](uint32_t v) {
        req[offset++] = static_cast<uint8_t>((v >> 24) & 0xFF);
        req[offset++] = static_cast<uint8_t>((v >> 16) & 0xFF);
        req[offset++] = static_cast<uint8_t>((v >> 8) & 0xFF);
        req[offset++] = static_cast<uint8_t>(v & 0xFF);
    };
    auto put16 = [&](uint16_t v) {
        req[offset++] = static_cast<uint8_t>((v >> 8) & 0xFF);
        req[offset++] = static_cast<uint8_t>(v & 0xFF);
    };
    auto put_bytes = [&](const uint8_t* data, size_t len) {
        std::memcpy(req.data() + offset, data, len);
        offset += len;
    };

    uint32_t transaction_id = random_u32();
    put64(connection_id);
    put32(1);
    put32(transaction_id);
    put_bytes(torrent.info_hash.data(), torrent.info_hash.size());

    std::array<uint8_t, 20> peer_bytes{};
    std::memcpy(peer_bytes.data(), peer_id.data(),
                std::min(peer_bytes.size(), peer_id.size()));
    put_bytes(peer_bytes.data(), peer_bytes.size());

    auto clamp_u64 = [](int64_t v) -> uint64_t {
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    };
    int64_t left = std::max<int64_t>(torrent.total_length() - downloaded, 0);

    put64(clamp_u64(downloaded));
    put64(static_cast<uint64_t>(left));
    put64(clamp_u64(uploaded));
    put32(udp_event_code(event));
    put32(0);  // IP address placeholder
    put32(random_u32());
    put32(0xFFFFFFFFu);
    put16(listen_port);

    std::vector<uint8_t> resp(64 * 1024);
    for (int attempt = 0; attempt < 3; ++attempt) {
        send_all(sock, req.data(), offset);
        int timeout_ms = 750 * (1 << attempt);
        ssize_t n = recv_with_timeout(sock, resp.data(), resp.size(), timeout_ms);
        if (n < 20) {
            continue;
        }
        uint32_t action = read_be32(resp.data());
        uint32_t resp_tx = read_be32(resp.data() + 4);
        if (action != 1 || resp_tx != transaction_id) {
            continue;
        }
        AnnounceResponse out;
        out.interval = static_cast<int>(read_be32(resp.data() + 8));
        out.incomplete = static_cast<int64_t>(read_be32(resp.data() + 12));
        out.complete = static_cast<int64_t>(read_be32(resp.data() + 16));
        size_t peers_len = static_cast<size_t>(n - 20);
        out.peers = parse_compact_peers_bytes(
            reinterpret_cast<const char*>(resp.data() + 20), peers_len);
        return out;
    }
    throw std::runtime_error("UDP tracker announce timed out");
}


TrackerClient::TrackerClient(std::string peer_id, uint16_t port)
    : peer_id_(std::move(peer_id)), port_(port) {}

AnnounceResponse TrackerClient::announce(const std::string& announce_url,
                                         const TorrentFile& torrent,
                                         int64_t downloaded,
                                         int64_t uploaded,
                                         std::string event) {
    if (is_udp_tracker_url(announce_url)) {
        return announce_udp(announce_url, torrent, downloaded, uploaded, event);
    }
    return announce_http(announce_url, torrent, downloaded, uploaded, event);
}

AnnounceResponse TrackerClient::announce_http(const std::string& announce_url,
                                              const TorrentFile& torrent,
                                              int64_t downloaded,
                                              int64_t uploaded,
                                              const std::string& event) {
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

    constexpr int kTrackerTimeoutMs = 10'000;
    HttpResponse http_resp = http_get(url, path, {}, std::numeric_limits<std::size_t>::max(),
                                      kTrackerTimeoutMs);
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

AnnounceResponse TrackerClient::announce_udp(const std::string& announce_url,
                                             const TorrentFile& torrent,
                                             int64_t downloaded,
                                             int64_t uploaded,
                                             const std::string& event) {
    ParsedUdpUrl parsed = parse_udp_tracker_url(announce_url);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    int rc = ::getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("UDP tracker DNS failed: ") + gai_strerror(rc));
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);

    std::string last_error = "no usable address";
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            last_error = std::string("socket() failed: ") + std::strerror(errno);
            continue;
        }
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            last_error = std::string("connect() failed: ") + std::strerror(errno);
            ::close(sock);
            continue;
        }
        try {
            uint64_t connection_id = udp_tracker_connect(sock);
            AnnounceResponse resp = udp_tracker_announce(sock,
                                                         connection_id,
                                                         torrent,
                                                         peer_id_,
                                                         port_,
                                                         downloaded,
                                                         uploaded,
                                                         event);
            ::close(sock);
            return resp;
        }
        catch (const std::exception& ex) {
            last_error = ex.what();
            ::close(sock);
        }
    }

    throw std::runtime_error("UDP tracker failed: " + last_error);
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
