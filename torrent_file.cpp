#include "torrent_file.h"
#include <openssl/sha.h>
#include <sstream>

bool TorrentFile::parse(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open torrent file: " << file_path << std::endl;
        return false;
    }

    raw_content_ = std::string(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    std::cout << "Loaded torrent file (" << raw_content_.size() << " bytes)" << std::endl;

    auto root = BencodeParser::parse(raw_content_);

    if (!root || !root->is_dict()) {
        std::cerr << "Failed to parse bencode - root is not a dictionary" << std::endl;
        return false;
    }

    return extract_torrent_info(*root);
}

bool TorrentFile::extract_torrent_info(const BencodeValue& root) {
    if (!root.is_dict()) return false;

    const auto& root_dict = root.as_dict();

    auto announce_it = root_dict.find("announce");
    if (announce_it != root_dict.end() && announce_it->second->is_string()) {
        announce_url_ = announce_it->second->as_string();
    }

    auto info_it = root_dict.find("info");
    if (info_it == root_dict.end()) {
        std::cerr << "No 'info' dictionary found" << std::endl;
        return false;
    }

    info_hash_ = calculate_info_hash(raw_content_);
    return extract_info_dict(*info_it->second);
}

bool TorrentFile::extract_info_dict(const BencodeValue& info) {
    if (!info.is_dict()) return false;

    const auto& info_dict = info.as_dict();

    auto piece_length_it = info_dict.find("piece length");
    if (piece_length_it != info_dict.end() && piece_length_it->second->is_integer()) {
        piece_length_ = static_cast<size_t>(piece_length_it->second->as_integer());
    }

    auto pieces_it = info_dict.find("pieces");
    if (pieces_it != info_dict.end() && pieces_it->second->is_string()) {
        const std::string& pieces_data = pieces_it->second->as_string();
        for (size_t i = 0; i < pieces_data.length(); i += 20) {
            if (i + 20 <= pieces_data.length()) {
                piece_hashes_.push_back(pieces_data.substr(i, 20));
            }
        }
    }

    auto name_it = info_dict.find("name");
    auto length_it = info_dict.find("length");

    if (name_it != info_dict.end() && name_it->second->is_string() &&
        length_it != info_dict.end() && length_it->second->is_integer()) {

        FileInfo file;
        file.name_ = name_it->second->as_string();
        file.length_ = static_cast<size_t>(length_it->second->as_integer());
        files_.push_back(file);
    }

    return true;
}

std::string TorrentFile::calculate_info_hash(const std::string& torrent_data) {
    size_t info_start = torrent_data.find("4:info");
    if (info_start == std::string::npos) return "";

    info_start += 6;
    size_t pos = info_start;
    size_t dict_count = 1;

    if (torrent_data[pos] != 'd') return "";
    ++pos;

    while (pos < torrent_data.length() && dict_count > 0) {
        if (torrent_data[pos] == 'd') {
            ++dict_count;
        } else if (torrent_data[pos] == 'e') {
            --dict_count;
        }
        ++pos;
    }

    std::string info_data = torrent_data.substr(info_start, pos - info_start);

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(info_data.c_str()),
         info_data.length(), hash);

    return std::string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
}

const std::string& TorrentFile::get_announce_url() const { return announce_url_; }
const std::string& TorrentFile::get_info_hash() const { return info_hash_; }
size_t TorrentFile::get_piece_length() const { return piece_length_; }
const std::vector<std::string>& TorrentFile::get_piece_hashes() const { return piece_hashes_; }
const std::vector<TorrentFile::FileInfo>& TorrentFile::get_files() const { return files_; }

void TorrentFile::print_info() const {
    std::cout << "=== Torrent Info ===" << std::endl;
    std::cout << "Announce URL: " << announce_url_ << std::endl;
    std::cout << "Piece Length: " << piece_length_ << " bytes" << std::endl;
    std::cout << "Number of pieces: " << piece_hashes_.size() << std::endl;
    std::cout << "Files:" << std::endl;
    for (const auto& file : files_) {
        std::cout << "  - " << file.name_ << " (" << file.length_ << " bytes)" << std::endl;
    }
}