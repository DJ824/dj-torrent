// TorrentFile parses .torrent metadata and exposes key fields.
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class TorrentFile {
public:
    struct FileEntry {
        int64_t length{};
        std::filesystem::path path;
    };

    static TorrentFile load(const std::filesystem::path& path);

    int64_t total_length() const;
    std::string info_hash_hex() const;

    std::optional<std::string> announce_url;
    std::vector<std::string> announce_list;
    std::vector<std::string> web_seeds;
    std::string name;
    int64_t piece_length{};
    std::vector<std::array<uint8_t, 20>> piece_hashes;
    std::vector<FileEntry> files;
    std::array<uint8_t, 20> info_hash{};
    std::string info_bencoded;
};
