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

// Replace the calculate_info_hash method in torrent_file.cpp with this single function

std::string TorrentFile::calculate_info_hash(const std::string& torrent_data) {
    std::cout << "Calculating info hash from torrent data (" << torrent_data.length() << " bytes)" << std::endl;

    // Find the start of the info dictionary by looking for "4:infod"
    size_t info_key_pos = torrent_data.find("4:info");
    if (info_key_pos == std::string::npos) {
        std::cerr << "Could not find '4:info' key in torrent data" << std::endl;
        return "";
    }

    // The info dictionary starts right after "4:info"
    size_t info_start = info_key_pos + 6; // Skip "4:info"

    if (info_start >= torrent_data.length() || torrent_data[info_start] != 'd') {
        std::cerr << "Info dictionary does not start with 'd'" << std::endl;
        return "";
    }

    // Now parse through the info dictionary to find its end
    size_t pos = info_start + 1; // Skip the opening 'd'
    int dict_depth = 1;

    while (pos < torrent_data.length() && dict_depth > 0) {
        char c = torrent_data[pos];

        if (c == 'd') {
            // Start of nested dictionary
            dict_depth++;
            pos++;
        } else if (c == 'e') {
            // End of dictionary
            dict_depth--;
            pos++;
        } else if (c == 'l') {
            // Start of list - just move past it, don't affect dict depth
            pos++;
        } else if (c == 'i') {
            // Integer: format is i<number>e
            pos++; // Skip 'i'
            size_t end_pos = torrent_data.find('e', pos);
            if (end_pos == std::string::npos) {
                std::cerr << "Malformed integer in info dictionary" << std::endl;
                return "";
            }
            pos = end_pos + 1; // Skip to after the 'e'
        } else if (std::isdigit(c)) {
            // String: format is <length>:<string>
            size_t colon_pos = torrent_data.find(':', pos);
            if (colon_pos == std::string::npos) {
                std::cerr << "Malformed string length in info dictionary" << std::endl;
                return "";
            }

            // Parse the length
            std::string length_str = torrent_data.substr(pos, colon_pos - pos);
            size_t length;
            try {
                length = std::stoull(length_str);
            } catch (const std::exception& e) {
                std::cerr << "Invalid string length: " << length_str << std::endl;
                return "";
            }

            // Skip to after the string data
            pos = colon_pos + 1 + length;
        } else {
            std::cerr << "Unexpected character '" << c << "' in info dictionary at position " << pos << std::endl;
            return "";
        }
    }

    if (dict_depth != 0) {
        std::cerr << "Malformed info dictionary - unmatched braces (depth: " << dict_depth << ")" << std::endl;
        return "";
    }

    // Extract the complete info dictionary including the 'd' and 'e'
    std::string info_bencode = torrent_data.substr(info_start, pos - info_start);

    std::cout << "Info dictionary length: " << info_bencode.length() << " bytes" << std::endl;
    std::cout << "Info dictionary preview: " << info_bencode.substr(0, std::min(size_t(100), info_bencode.length())) << std::endl;

    // Calculate SHA1 hash of the info dictionary
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(info_bencode.c_str()),
         info_bencode.length(), hash);

    std::string hash_string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);

    // Debug output
    std::cout << "Calculated info hash (hex): ";
    for (unsigned char c : hash_string) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
    }
    std::cout << std::dec << std::endl;

    return hash_string;
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