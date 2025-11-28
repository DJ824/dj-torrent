// Minimal HTTP tracker client for BitTorrent announce requests.
#pragma once

#include "torrent_file.h"

#include <cstdint>
#include <string>
#include <vector>

struct PeerEndpoint {
    std::string ip;
    uint16_t port{};
};

struct AnnounceResponse {
    int interval{};
    int64_t complete{};
    int64_t incomplete{};
    std::vector<PeerEndpoint> peers;
};

class TrackerClient {
public:
    TrackerClient(std::string peer_id, uint16_t port);

    AnnounceResponse announce(const std::string& announce_url,
                              const TorrentFile& torrent,
                              int64_t downloaded = 0,
                              int64_t uploaded = 0,
                              std::string event = "started");

private:
    std::string peer_id_;
    uint16_t port_;
};

// Utility to produce a 20-byte peer id using a readable prefix.
std::string generate_peer_id(std::string prefix = "-DJ0001-");
