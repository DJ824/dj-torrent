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

#include "http_client.h"

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
      storage_(torrent_, std::move(download_path)) {
    logger_.start();
    piece_manager_.set_piece_complete_callback(
        [this](uint32_t piece_index, const std::vector<uint8_t>& data) {
            if (!storage_.write_piece(piece_index, data)) {
                logger_.error("Failed to write piece to storage");
            }
            handle_piece_complete(piece_index);
        });
}

void Session::start() {
    if (start_from_tracker()) {
        return;
    }
    if (start_from_web_seeds()) {
        return;
    }
    throw std::runtime_error(
        "Torrent does not include a usable HTTP(S) tracker or any web seeds (url-list)");
}

bool Session::start_from_tracker() {
    std::vector<std::string> urls = collect_tracker_urls();
    std::vector<std::string> http_urls;
    std::unordered_set<std::string> seen;
    for (const auto& url : urls) {
        if (!is_http_tracker(url)) continue;
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (seen.insert(lower).second) {
            http_urls.push_back(url);
        }
    }

    if (http_urls.empty()) {
        logger_.warn("no HTTP(S) trackers available in announce or announce-list");
        return false;
    }

    for (const auto& url : http_urls) {
        try {
            logger_.info(std::string("contacting tracker: ") + url);
            auto res = tracker_client_.announce(url, torrent_);
            if (res.peers.empty()) {
                logger_.warn(std::string("tracker returned zero peers: ") + url);
                continue;
            }
            for (const auto& ep : res.peers) {
                add_peer(PeerAddress{ep.ip, ep.port});
            }
            return true;
        } catch (const std::exception& ex) {
            logger_.warn(std::string("tracker failed: ") + ex.what());
        }
    }
    logger_.warn("all HTTP(S) trackers failed");
    return false;
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
    } catch (const std::exception& ex) {
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

void Session::add_peer(const PeerAddress& address) {
    try {
        Peer peer = Peer::connect_outgoing(address, torrent_.info_hash, self_peer_id_);
        int fd = peer.fd();
        if (event_loop_.add_peer(std::move(peer))) {
            ensure_peer_state(fd);
        }
    } catch (const std::exception& ex) {
        logger_.error("Failed to connect to peer");
    }
}

void Session::run_once(int timeout_ms) { event_loop_.run_once(timeout_ms); }

void Session::run(int timeout_ms) { event_loop_.run(timeout_ms); }

std::size_t Session::peer_count() const { return event_loop_.peer_count(); }

void Session::handle_peer_events(Peer& peer, std::vector<Peer::Event>&& events) {
    PeerState& state = ensure_peer_state(peer.fd());

    for (auto& ev : events) {
        switch (ev.type) {
        case Peer::EventType::Handshake:
                logger_.info("received handshake, sending our bitfield");
                state.remote_id = ev.peer_id;
                peer.send_bitfield(piece_manager_.have_bitfield());
                break;
        case Peer::EventType::Bitfield:
                logger_.info("received bitfield");
                state.bitfield = ev.payload;
                state.bitfield.resize(piece_manager_.have_bitfield().size());
                break;
        case Peer::EventType::Have:
                logger_.info("received bitfield have");
                if (ev.piece_index / 8 >= state.bitfield.size()) {
                    state.bitfield.resize(piece_manager_.have_bitfield().size());
                }
                bitfield_set(state.bitfield, ev.piece_index);
                break;
        case Peer::EventType::Choke:
                logger_.info("peer choking us");
                state.choked = true;
                break;
        case Peer::EventType::Unchoke:
            logger_.info("peer unchoking us");
                state.choked = false;
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
                logger_.info(std::string_view(buf, std::strlen(buf)));
            }
            if (piece_manager_.handle_block(ev.piece_index, ev.begin, ev.payload)) {
                if (state.inflight_requests > 0) {
                    --state.inflight_requests;
                }
            }
            break;
            case Peer::EventType::Request: {
                    char buf[128];
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "received request piece=%u begin=%u len=%u",
                                  ev.piece_index,
                                  ev.begin,
                                  ev.length);
                    logger_.info(std::string_view(buf, std::strlen(buf)));
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
            default:
                break;
        }
    }

    maybe_request(peer, state);

    if (peer.is_closed()) {
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
    } else if (!interesting && state.interested) {
        peer.send_not_interested();
        state.interested = false;
    }

    if (state.choked) {
        return;
    }

    constexpr uint32_t kMaxInflightRequestsPerPeer = 16;

    while (state.inflight_requests < kMaxInflightRequestsPerPeer) {
        auto req = piece_manager_.next_request_for_peer(state.bitfield);
        if (!req) {
            break;
        }
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
