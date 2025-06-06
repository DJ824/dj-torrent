#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>
#include "bencode_parser.h"

class TorrentFile {
public:
    struct FileInfo {
        std::string name_;
        size_t length_;
        std::vector<std::string> path_;
    };

private:
    std::string announce_url_;
    std::vector<FileInfo> files_;
    std::string info_hash_;
    size_t piece_length_;
    std::vector<std::string> piece_hashes_;
    std::string raw_content_;

public:
    bool parse(const std::string& file_path);
    const std::string& get_announce_url() const;
    const std::string& get_info_hash() const;
    size_t get_piece_length() const;
    const std::vector<std::string>& get_piece_hashes() const;
    const std::vector<FileInfo>& get_files() const;
    void print_info() const;

private:
    bool extract_torrent_info(const BencodeValue& root);
    bool extract_info_dict(const BencodeValue& info);
    std::string calculate_info_hash(const std::string& torrent_data);
};