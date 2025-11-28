#include "torrent_file.h"

#include "bencode.h"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <openssl/sha.h>
#include <stdexcept>

namespace {
    std::string read_file_to_string(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        return data;
    }

    std::string to_hex(const std::array<uint8_t, 20>& bytes) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(40);
        for (uint8_t b : bytes) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0xF]);
        }
        return out;
    }

    std::filesystem::path join_path(const bencode::List& components) {
        std::filesystem::path p;
        for (const auto& v : components) {
            const auto& part = bencode::as_string(v);
            p /= part;
        }
        return p;
    }
} // namespace

TorrentFile TorrentFile::load(const std::filesystem::path& path) {
    std::string raw = read_file_to_string(path);

    bencode::Parser parser(raw, "info");
    bencode::Value root_v = parser.parse();
    auto span = parser.tracked_span();
    if (!span) {
        throw std::runtime_error(
            "Failed to locate bencoded info dictionary for hashing");
    }
    TorrentFile t;
    t.info_bencoded = raw.substr(span->first, span->second);

    const auto& root = bencode::as_dict(root_v);
    if (const auto* announce = bencode::find_field(root, "announce")) {
        t.announce_url = bencode::as_string(*announce);
    }

    if (const auto* list_field = bencode::find_field(root, "announce-list")) {
        const auto& tiers = bencode::as_list(*list_field);
        for (const auto& tier_v : tiers) {
            const auto& tier_list = bencode::as_list(tier_v);
            for (const auto& url_v : tier_list) {
                t.announce_list.push_back(bencode::as_string(url_v));
            }
        }
    }

    if (const auto* url_list_field = bencode::find_field(root, "url-list")) {
        if (std::holds_alternative<std::string>(url_list_field->data)) {
            t.web_seeds.push_back(bencode::as_string(*url_list_field));
        }
        else {
            const auto& seeds = bencode::as_list(*url_list_field);
            for (const auto& seed_v : seeds) {
                t.web_seeds.push_back(bencode::as_string(seed_v));
            }
        }
    }

    const auto& info = bencode::as_dict(bencode::require_field(root, "info"));
    t.name = bencode::as_string(bencode::require_field(info, "name"));
    t.piece_length =
        bencode::as_int(bencode::require_field(info, "piece length"));
    const auto& pieces_blob =
        bencode::as_string(bencode::require_field(info, "pieces"));
    if (pieces_blob.size() % 20 != 0) {
        throw std::runtime_error("Pieces field size is not a multiple of 20 bytes");
    }
    size_t piece_count = pieces_blob.size() / 20;
    t.piece_hashes.reserve(piece_count);
    for (size_t i = 0; i < piece_count; ++i) {
        std::array<uint8_t, 20> hash{};
        std::copy_n(reinterpret_cast<const uint8_t*>(pieces_blob.data()) + i * 20,
                    20, hash.data());
        t.piece_hashes.push_back(hash);
    }

    if (const auto* files_field = bencode::find_field(info, "files")) {
        const auto& files_list = bencode::as_list(*files_field);
        for (const auto& f_v : files_list) {
            const auto& f_dict = bencode::as_dict(f_v);
            int64_t length =
                bencode::as_int(bencode::require_field(f_dict, "length"));
            const auto& path_list =
                bencode::as_list(bencode::require_field(f_dict, "path"));
            t.files.push_back(FileEntry{length, join_path(path_list)});
        }
    }
    else {
        int64_t length = bencode::as_int(bencode::require_field(info, "length"));
        t.files.push_back(FileEntry{length, std::filesystem::path(t.name)});
    }

    SHA1(reinterpret_cast<const unsigned char*>(t.info_bencoded.data()),
         t.info_bencoded.size(), t.info_hash.data());

    return t;
}

int64_t TorrentFile::total_length() const {
    return std::accumulate(
        files.begin(), files.end(), int64_t{0},
        [](int64_t acc, const FileEntry& f) { return acc + f.length; });
}

std::string TorrentFile::info_hash_hex() const { return to_hex(info_hash); }
