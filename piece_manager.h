#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <mutex>
#include <openssl/sha.h>
#include "torrent_file.h"

class PieceManager {
public:
    enum PieceState {
        NEEDED,
        REQUESTED,
        DOWNLOADING,
        COMPLETE
    };

private:
    TorrentFile* torrent_;
    std::vector<PieceState> piece_states_;
    std::vector<std::vector<char>> piece_data_;
    std::vector<bool> piece_verified_;
    std::string download_path_;
    std::unique_ptr<std::fstream> file_stream_;
    mutable std::mutex pieces_mutex_;
    size_t pieces_downloaded_ = 0;
    size_t total_pieces_ = 0;
    size_t bytes_downloaded_ = 0;
    size_t total_bytes_ = 0;

    static const size_t BLOCK_SIZE = 16384;

    struct PieceProgress {
        std::vector<bool> blocks_received_;
        size_t blocks_needed_;
        size_t blocks_downloaded_;
        PieceProgress() : blocks_needed_(0), blocks_downloaded_(0) {}

        bool is_complete() const {
            return blocks_downloaded_ == blocks_needed_;
        }

        void initialize(size_t piece_size) {
            blocks_needed_ = (piece_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
            blocks_received_.resize(blocks_needed_, false);
            blocks_downloaded_ = 0;
        }
    };

    std::vector<PieceProgress> piece_progress_;

public:
    explicit PieceManager(TorrentFile* torrent, std::string download_path = "./");
    ~PieceManager();
    int get_next_needed_piece();
    bool request_piece(int piece_index);
    bool add_piece_block(int piece_index, int block_offset, const std::vector<char>& block_data);
    bool add_complete_piece(int piece_index, const std::vector<char>& piece_data);
    bool has_piece(int piece_index) const;
    bool is_complete() const;
    PieceState get_piece_state(int piece_index) const;
    std::vector<char> get_piece_data(int piece_index);
    std::vector<char> get_block_data(int piece_index, int block_offset, int block_length);
    double get_completion_percentage() const;
    size_t get_bytes_downloaded() const;
    size_t get_total_bytes() const;
    std::vector<std::pair<int, int>> get_missing_blocks(int piece_index) const;
    void print_progress() const;

private:
    bool complete_piece(int piece_index);
    void reset_piece_progress(int piece_index);
    bool initialize_file();
    bool verify_piece(int piece_index);
    bool write_piece_to_disk(int piece_index);
    static std::string calculate_piece_hash(const std::vector<char>& piece_data);
    size_t get_piece_size(int piece_index) const;
    bool is_valid_piece_index(int piece_index) const;
    size_t get_block_index(size_t block_offset) const;
    size_t get_block_size(int piece_index, size_t block_index) const;
};