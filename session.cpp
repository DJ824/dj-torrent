#include "session.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>

#include "http_client.h"

std::string format_peer_id_hex(const std::string& peer_id) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(peer_id.size() * 2);
    for (unsigned char c : peer_id) {
        out.push_back(kHexDigits[c >> 4]);
        out.push_back(kHexDigits[c & 0x0F]);
    }
    return out;
}

int make_listen_socket(uint16_t port) {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 128) < 0) {
        ::close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}


static std::size_t piece_count(const TorrentFile& t) {
    return t.piece_hashes.size();
}

Session::Session(TorrentFile torrent,
                 std::string peer_id,
                 uint16_t listen_port,
                 std::size_t block_size,
                 std::filesystem::path download_path)
    : torrent_(std::move(torrent)),
      self_peer_id_(std::move(peer_id)),
      listen_port_(listen_port),
      block_size_(block_size),
      tracker_client_(self_peer_id_, listen_port_),
      piece_manager_(torrent_, block_size),
      event_loop_([this](Peer& peer, std::vector<Peer::Event>&& events) {
          handle_peer_events(peer, std::move(events));
      }),
      storage_(torrent_, download_path) {
    logger_.start();
    piece_manager_.set_piece_complete_callback(
        [this](uint32_t piece_index, const std::vector<uint8_t>& data) {
            if (!storage_.write_piece(piece_index, data)) {
                logger_.error("failed to write piece");
            }
            handle_piece_complete(piece_index);
        });

    int listen_fd = make_listen_socket(listen_port_);
    if (listen_fd >= 0) {
        event_loop_.set_listen_socket(
            listen_fd,
            [this](int fd, const PeerAddress& addr) {
                try {
                    Peer peer =
                        Peer::from_incoming(fd, addr, torrent_.info_hash, self_peer_id_);
                    int pfd = peer.fd();
                    if (event_loop_.add_peer(std::move(peer))) {
                        ensure_peer_state(pfd);
                    } else {
                        ::close(fd);
                    }
                } catch (...) {
                    ::close(fd);
                }
            });
    }
}

Session::~Session() {
    stop();
    logger_.stop();
}


void Session::start() {
    if (start_from_tracker()) {
        return;
    }
    if (start_from_web_seeds()) {
        return;
    }
    throw std::runtime_error(
        "torrent does not include a usable HTTP(S)/UDP tracker or any web seeds (url-list)");
}

bool Session::start_from_tracker() {
    std::vector<std::string> urls = collect_tracker_urls();
    std::vector<std::string> tracker_urls;
    std::unordered_set<std::string> seen;
    for (const auto& url : urls) {
        if (!is_http_tracker(url) && !is_udp_tracker(url)) continue;
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (seen.insert(lower).second) {
            tracker_urls.push_back(url);
        }
    }

    if (tracker_urls.empty()) {
        logger_.warn("no HTTP(S) or UDP trackers available in announce or announce-list");
        return false;
    }

    if (!tracker_thread_.joinable()) {
        tracker_stop_.store(false, std::memory_order_relaxed);
        tracker_thread_ = std::thread([this, tracker_urls = std::move(tracker_urls)]() {
            tracker_worker(tracker_urls);
        });
    }
    return true;
}

bool Session::start_from_web_seeds() {
    for (const auto& base : torrent_.web_seeds) {
        if (try_download_from_web_seed(base)) {
            return true;
        }
    }
    return false;
}

bool Session::try_download_from_web_seed(const std::string& base_url) {
    try {
        std::string file_url = build_web_seed_url(base_url);
        logger_.info(std::string("using web seed: ") + file_url);

        HttpUrl parsed = parse_http_url(file_url);
        std::size_t pieces = torrent_.piece_hashes.size();
        for (uint32_t idx = 0; idx < pieces; ++idx) {
            uint32_t len = piece_length(idx);
            uint64_t offset =
                static_cast<uint64_t>(idx) * static_cast<uint64_t>(torrent_.piece_length);
            std::string range_value = "bytes=" + std::to_string(offset) + "-" +
                std::to_string(offset + static_cast<uint64_t>(len) - 1);

            HttpResponse resp = http_get(parsed,
                                         parsed.path,
                                         {{"Range", range_value}},
                                         static_cast<std::size_t>(len) + 16 * 1024);
            if (resp.status_code != 206 && resp.status_code != 200) {
                std::ostringstream oss;
                oss << "unexpected status for piece " << idx << ": " << resp.status_line;
                throw std::runtime_error(oss.str());
            }
            if (resp.body.size() < len) {
                throw std::runtime_error("short body from web seed");
            }
            if (resp.body.size() > len) {
                throw std::runtime_error("web seed returned larger body than expected");
            }

            for (uint32_t begin = 0; begin < len; begin += static_cast<uint32_t>(block_size_)) {
                uint32_t take = static_cast<uint32_t>(
                    std::min<std::size_t>(block_size_, static_cast<std::size_t>(len - begin)));
                std::vector<uint8_t> chunk(resp.body.begin() + begin,
                                           resp.body.begin() + begin + take);
                if (!piece_manager_.handle_block(idx, begin, chunk)) {
                    throw std::runtime_error("failed to accept block from web seed");
                }
            }
        }
        return true;
    }
    catch (const std::exception& ex) {
        logger_.error(std::string("web seed failed: ") + ex.what());
        return false;
    }
}

std::string Session::build_web_seed_url(const std::string& base) const {
    if (base.empty()) {
        return base;
    }
    if (base.size() >= torrent_.name.size() &&
        base.compare(base.size() - torrent_.name.size(), torrent_.name.size(), torrent_.name) ==
        0) {
        return base;
    }
    if (base.back() == '/') {
        return base + torrent_.name;
    }
    return base + "/" + torrent_.name;
}

std::vector<std::string> Session::collect_tracker_urls() const {
    std::vector<std::string> urls;
    if (torrent_.announce_url) {
        urls.push_back(*torrent_.announce_url);
    }
    urls.insert(urls.end(), torrent_.announce_list.begin(), torrent_.announce_list.end());
    return urls;
}

bool Session::is_http_tracker(const std::string& url) {
    auto lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool Session::is_udp_tracker(const std::string& url) {
    auto lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("udp://", 0) == 0;
}

void Session::add_peer(const PeerAddress& address) {
    enqueue_peer_candidate(address);
}

void Session::run_once(int timeout_ms) {
    maybe_connect_pending_peers();
    event_loop_.run_once(timeout_ms);

    maybe_connect_pending_peers();
    maybe_drop_handshake_timeouts();
    maybe_log_stats();
}

void Session::run(int timeout_ms) {
    running_.store(true, std::memory_order_relaxed);
    while (running_.load(std::memory_order_relaxed)) {
        run_once(timeout_ms);
    }
}

void Session::stop() {
    running_.store(false, std::memory_order_relaxed);
    event_loop_.stop();
    stop_tracker_thread();
}

std::size_t Session::peer_count() const { return event_loop_.peer_count(); }

void Session::connect_peer_now(const PeerAddress& address) {
    try {
        Peer peer = Peer::connect_outgoing(address, torrent_.info_hash, self_peer_id_);
        int fd = peer.fd();
        if (event_loop_.add_peer(std::move(peer))) {
            ensure_peer_state(fd);
        }
    }
    catch (const std::exception& ex) {
        logger_.error("Failed to connect to peer");
    }
}

void Session::maybe_connect_pending_peers() {
    static constexpr std::size_t kMaxActivePeers = 50;
    while (peer_count() < kMaxActivePeers) {
        PeerAddress next;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_peers_.empty()) {
                break;
            }
            next = pending_peers_.front();
            pending_peers_.pop_front();
        }
        connect_peer_now(next);
    }
}

void Session::maybe_log_stats() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    if (last_stats_log_.time_since_epoch().count() == 0) {
        last_stats_log_ = now;
        return;
    }
    if (now - last_stats_log_ < seconds(5)) {
        return;
    }
    last_stats_log_ = now;
    std::size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending = pending_peers_.size();
    }
    std::string msg = "stats: active_peers=" + std::to_string(peer_count()) +
        " pending_peers=" + std::to_string(pending) +
        " pex_peers_discovered=" + std::to_string(pex_peers_discovered_);
    logger_.info(msg);
}

void Session::maybe_drop_handshake_timeouts() {
    using namespace std::chrono;
    static constexpr auto kHandshakeTimeout = seconds(2);
    auto now = steady_clock::now();
    std::vector<int> drop_fds;
    for (auto& kv : peers_) {
        const auto& state = kv.second;
        if (state.handshake_received) {
            continue;
        }
        if (state.connected_at.time_since_epoch().count() == 0) {
            continue;
        }
        if (now - state.connected_at > kHandshakeTimeout) {
            drop_fds.push_back(kv.first);
        }
    }
    for (int fd : drop_fds) {
        if (Peer* peer = event_loop_.peer_by_fd(fd)) {
            logger_.warn(std::string("peer handshake timeout for ") + peer->remote().ip);
            peer->handle_error();
        }
        event_loop_.remove_peer(fd);
        peers_.erase(fd);
    }
}

bool Session::enqueue_peer_candidate(const PeerAddress& address) {
    std::string key = address.ip + ":" + std::to_string(address.port);
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!known_endpoints_.insert(std::move(key)).second) {
        return false;
    }
    pending_peers_.push_back(address);
    return true;
}

void Session::tracker_worker(std::vector<std::string> tracker_urls) {
    bool any_success = false;
    for (const auto& url : tracker_urls) {
        if (tracker_stop_.load(std::memory_order_relaxed)) {
            break;
        }
        try {
            logger_.info(std::string("contacting tracker: ") + url);
            auto res = tracker_client_.announce(url, torrent_);
            if (res.peers.empty()) {
                logger_.warn(std::string("tracker returned zero peers: ") + url);
                continue;
            }
            logger_.info(std::string("tracker ") + url + " returned " +
                         std::to_string(res.peers.size()) + " peers");
            any_success = true;
            for (const auto& ep : res.peers) {
                enqueue_peer_candidate(PeerAddress{ep.ip, ep.port});
            }
        }
        catch (const std::exception& ex) {
            logger_.warn(std::string("tracker failed: ") + ex.what());
        }
    }
    if (!any_success) {
        logger_.warn("all trackers failed or returned zero peers");
    }
}

void Session::stop_tracker_thread() {
    tracker_stop_.store(true, std::memory_order_relaxed);
    if (tracker_thread_.joinable()) {
        tracker_thread_.join();
    }
}

void Session::handle_peer_events(Peer& peer, std::vector<Peer::Event>&& events) {
    PeerState& state = ensure_peer_state(peer.fd());

    for (auto& ev : events) {
        switch (ev.type) {
        case Peer::EventType::Handshake:
            state.remote_id = ev.peer_id;
            state.handshake_received = true;
            {
                std::string msg = "received handshake from peer " + peer.remote().ip +
                    ", sending our bitfield";
                logger_.info(msg);
            }
            peer.send_bitfield(piece_manager_.have_bitfield());
            peer.send_extended_handshake();
            break;
        case Peer::EventType::Bitfield:
            {
                std::string msg = "received bitfield from peer " + peer.remote().ip;
                logger_.info(msg);
                state.bitfield = ev.payload;
                size_t n = piece_count(torrent_);
                for (size_t i = 0; i < n; i++) {
                    if (bitfield_test(state.bitfield, i)) {
                        piece_manager_.sum_peer_bitfield_ct_[i]++;
                    }
                }
                piece_manager_.update_buckets();
                state.bitfield.resize(piece_manager_.have_bitfield().size());
            }
            break;
        case Peer::EventType::ExtendedHandshake:
            break;
        case Peer::EventType::Have:
            {
                std::string msg = "received have for piece " +
                    std::to_string(ev.piece_index) + " from peer " + peer.remote().ip;
                logger_.info(msg);
                if (ev.piece_index / 8 >= state.bitfield.size()) {
                    state.bitfield.resize(piece_manager_.have_bitfield().size());
                }
                piece_manager_.sum_peer_bitfield_ct_[ev.piece_index]++;
                // todo o(n) prolly not needed here
                piece_manager_.update_buckets();
                bitfield_set(state.bitfield, ev.piece_index);
            }
            break;
        case Peer::EventType::Choke:
            {
                std::string msg = "peer " + peer.remote().ip + " choking us";
                logger_.info(msg);
                state.choked = true;
            }
            break;
        case Peer::EventType::Unchoke:
            {
                std::string msg = "peer " + peer.remote().ip + " unchoking us";
                logger_.info(msg);
                state.choked = false;
            }
            break;
        case Peer::EventType::Piece:
            {
                char buf[128];
                std::snprintf(buf,
                              sizeof(buf),
                              "received piece idx=%u begin=%u len=%u",
                              ev.piece_index,
                              ev.begin,
                              ev.length);
                std::string msg = "peer " + peer.remote().ip + " " + std::string(buf);
                logger_.info(msg);
            }
            if (state.inflight_requests > 0) {
                --state.inflight_requests;
            }
            (void)piece_manager_.handle_block(ev.piece_index, ev.begin, ev.payload);
            break;
        case Peer::EventType::Request:
            {
                char buf[128];
                std::snprintf(buf,
                              sizeof(buf),
                              "received request piece=%u begin=%u len=%u",
                              ev.piece_index,
                              ev.begin,
                              ev.length);
                std::string msg = "peer " + peer.remote().ip + " " + std::string(buf);
                logger_.info(msg);
                if (!piece_manager_.have_piece(ev.piece_index)) {
                    break;
                }
                if (ev.begin + ev.length > piece_length(ev.piece_index)) {
                    break;
                }
                auto block = storage_.read_block(ev.piece_index, ev.begin, ev.length);
                if (block) {
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "fulfilling request piece=%u begin=%u len=%u",
                                  ev.piece_index,
                                  ev.begin,
                                  ev.length);
                    logger_.info(std::string_view(buf, std::strlen(buf)));
                    peer.send_piece(ev.piece_index, ev.begin, *block);
                }
                break;
            }
        case Peer::EventType::Pex:
            handle_pex(peer, ev.payload);
            break;
        default:
            break;
        }
    }

    maybe_request(peer, state);

    if (peer.is_closed()) {
        std::string msg = "peer " + peer.remote().ip + " closed connection";
        logger_.info(msg);
        peers_.erase(peer.fd());
    }
}

void Session::handle_piece_complete(uint32_t piece_index) {
    event_loop_.for_each_peer([piece_index](Peer& p) { p.send_have(piece_index); });
}

void Session::maybe_request(Peer& peer, PeerState& state) {
    bool interesting = peer_has_interesting(state);
    if (interesting && !state.interested) {
        peer.send_interested();
        state.interested = true;
        std::string msg = "sending interested to peer " + peer.remote().ip;
        logger_.info(msg);
    }
    else if (!interesting && state.interested) {
        peer.send_not_interested();
        state.interested = false;
        std::string msg = "sending not interested to peer " + peer.remote().ip;
        logger_.info(msg);
    }

    if (state.choked) {
        return;
    }

    constexpr uint32_t kMaxInflightRequestsPerPeer = 32;

    while (state.inflight_requests < kMaxInflightRequestsPerPeer) {
        auto req = piece_manager_.next_request_for_peer_rarest(state.bitfield);
        if (!req) {
            break;
        }
        std::string msg = "sending request to peer " + peer.remote().ip +
            " piece=" + std::to_string(req->piece_index) +
            " begin=" + std::to_string(req->begin) +
            " len=" + std::to_string(req->length);
        logger_.info(msg);
        peer.send_request(req->piece_index, req->begin, req->length);
        ++state.inflight_requests;
    }
}

bool Session::peer_has_interesting(const PeerState& state) const {
    std::size_t total = piece_count(torrent_);
    for (uint32_t i = 0; i < total; ++i) {
        if (!piece_manager_.have_piece(i) && bitfield_test(state.bitfield, i)) {
            return true;
        }
    }
    return false;
}

Session::PeerState& Session::ensure_peer_state(int fd) {
    auto it = peers_.find(fd);
    if (it != peers_.end()) {
        if (it->second.bitfield.empty()) {
            it->second.bitfield = make_bitfield(piece_count(torrent_));
        }
        return it->second;
    }
    PeerState state;
    state.bitfield = make_bitfield(piece_count(torrent_));
    state.connected_at = std::chrono::steady_clock::now();
    auto [inserted_it, _] = peers_.emplace(fd, std::move(state));
    return inserted_it->second;
}

uint32_t Session::piece_length(uint32_t piece_index) const {
    if (piece_index + 1 == torrent_.piece_hashes.size()) {
        int64_t full =
            torrent_.piece_length * static_cast<int64_t>(torrent_.piece_hashes.size() - 1);
        return static_cast<uint32_t>(torrent_.total_length() - full);
    }
    return static_cast<uint32_t>(torrent_.piece_length);
}

std::vector<uint8_t> Session::make_bitfield(std::size_t pieces) {
    return std::vector<uint8_t>((pieces + 7) / 8, 0);
}

void Session::bitfield_set(std::vector<uint8_t>& bf, uint32_t idx) {
    uint32_t byte = idx / 8;
    uint32_t bit = 7 - (idx % 8);
    if (byte >= bf.size()) {
        return;
    }
    bf[byte] |= static_cast<uint8_t>(1u << bit);
}

bool Session::bitfield_test(const std::vector<uint8_t>& bf, uint32_t idx) {
    uint32_t byte = idx / 8;
    uint32_t bit = 7 - (idx % 8);
    if (byte >= bf.size()) {
        return false;
    }
    return (bf[byte] & (1u << bit)) != 0;
}
