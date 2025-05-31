#include <filesystem>
#include <iomanip>
#include <algorithm>
#include "piece_manager.h"

PieceManager::PieceManager(TorrentFile* torrent, std::string download_path)
    : torrent_(torrent), download_path_(std::move(download_path)) {

    if (!torrent) {
        throw std::invalid_argument("torrent cannot be null");
    }

    total_pieces_ = torrent_->get_piece_hashes().size();
    piece_states_.resize(total_pieces_, NEEDED);
    piece_data_.resize(total_pieces_);
    piece_verified_.resize(total_pieces_, false);

    for (const auto& file : torrent_->get_files()) {
        total_bytes_ += file.length_;
    }

    for (size_t i = 0; i < total_pieces_; ++i) {
        piece_data_[i].resize(get_piece_size(i));
    }

    piece_progress_.resize(total_pieces_);
    for (size_t i = 0; i < total_pieces_; ++i) {
        piece_progress_[i].initialize(get_piece_size(i));
    }

    std::cout << "piece manager initialized with " << total_pieces_ << " pieces, "
              << total_bytes_ << " total bytes" << std::endl;

    if (!initialize_file()) {
        throw std::runtime_error("failed to initialize output file");
    }
}

PieceManager::~PieceManager() {
    if (file_stream_ && file_stream_->is_open()) {
        file_stream_->close();
    }
}

// setup location to write to disk
bool PieceManager::initialize_file() {
    std::filesystem::create_directories(download_path_);

    std::string file_path;
    if (!torrent_->get_files().empty()) {
        file_path = download_path_ + "/" + torrent_->get_files()[0].name_;
    } else {
        file_path = download_path_ + "/download.bin";
    }

    std::cout << "initializing file: " << file_path << std::endl;

    file_stream_ = std::make_unique<std::fstream>(
        file_path,
        std::ios::in | std::ios::out | std::ios::binary
    );

    if (!file_stream_->is_open()) {
        file_stream_ = std::make_unique<std::fstream>(
            file_path,
            std::ios::out | std::ios::binary
        );
        file_stream_->close();

        file_stream_ = std::make_unique<std::fstream>(
            file_path,
            std::ios::in | std::ios::out | std::ios::binary
        );
    }

    if (!file_stream_->is_open()) {
        std::cerr << "failed to open file: " << file_path << std::endl;
        return false;
    }

    file_stream_->seekp(total_bytes_ - 1);
    file_stream_->write("", 1);
    file_stream_->flush();
    return true;
}

int PieceManager::get_next_needed_piece() {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    for (size_t i = 0; i < piece_states_.size(); ++i) {
        if (piece_states_[i] == NEEDED) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool PieceManager::request_piece(int piece_index) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index)) {
        return false;
    }

    if (piece_states_[piece_index] == NEEDED) {
        piece_states_[piece_index] = REQUESTED;
        std::cout << "requested piece " << piece_index << std::endl;
        return true;
    }
    return false;
}

bool PieceManager::add_piece_block(int piece_index, int block_offset, const std::vector<char>& block_data) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index)) {
        std::cerr << "invalid piece index: " << piece_index << std::endl;
        return false;
    }

    if (piece_states_[piece_index] == COMPLETE) {
        std::cout << "already have piece " << piece_index << std::endl;
        return true;
    }

    size_t piece_size = get_piece_size(piece_index);

    if (block_offset < 0 ||
        static_cast<size_t>(block_offset) >= piece_size ||
        static_cast<size_t>(block_offset + block_data.size()) > piece_size) {
        std::cerr << "invalid block offset/size for piece " << piece_index
                  << " (offset: " << block_offset << ", size: " << block_data.size()
                  << ", piece_size: " << piece_size << ")" << std::endl;
        return false;
    }

    size_t block_index = get_block_index(block_offset);
    size_t expected_block_size = get_block_size(piece_index, block_index);

    if (block_data.size() != expected_block_size) {
        std::cerr << "block size mismatch for piece " << piece_index
                  << ", block " << block_index
                  << " (expected: " << expected_block_size
                  << ", got: " << block_data.size() << ")" << std::endl;
        return false;
    }

    if (piece_progress_[piece_index].blocks_received_[block_index]) {
        std::cout << "already have block " << block_index << " of piece " << piece_index << std::endl;
        return true;
    }

    std::copy(block_data.begin(), block_data.end(),
              piece_data_[piece_index].begin() + block_offset);

    piece_progress_[piece_index].blocks_received_[block_index] = true;
    piece_progress_[piece_index].blocks_downloaded_++;
    piece_states_[piece_index] = DOWNLOADING;

    std::cout << "received block " << block_index << " of piece " << piece_index
              << " (" << piece_progress_[piece_index].blocks_downloaded_
              << "/" << piece_progress_[piece_index].blocks_needed_ << " blocks)" << std::endl;

    if (piece_progress_[piece_index].is_complete()) {
        std::cout << "all blocks received for piece " << piece_index << ", completing..." << std::endl;
        return complete_piece(piece_index);
    }

    return true;
}

// verifies the piece hash and writes to disk
bool PieceManager::complete_piece(int piece_index) {
    if (!is_valid_piece_index(piece_index)) {
        return false;
    }

    if (!verify_piece(piece_index)) {
        std::cerr << "piece " << piece_index << " failed SHA-1 verification" << std::endl;
        reset_piece_progress(piece_index);
        return false;
    }

    if (!write_piece_to_disk(piece_index)) {
        std::cerr << "failed to write piece " << piece_index << " to disk" << std::endl;
        piece_states_[piece_index] = DOWNLOADING;
        return false;
    }

    piece_states_[piece_index] = COMPLETE;
    piece_verified_[piece_index] = true;
    pieces_downloaded_++;
    bytes_downloaded_ += get_piece_size(piece_index);

    std::cout << "piece " << piece_index << " completed and written to disk" << std::endl;
    print_progress();

    return true;
}

void PieceManager::reset_piece_progress(int piece_index) {
    if (!is_valid_piece_index(piece_index)) return;

    piece_states_[piece_index] = NEEDED;

    auto& progress = piece_progress_[piece_index];
    std::fill(progress.blocks_received_.begin(), progress.blocks_received_.end(), false);
    progress.blocks_downloaded_ = 0;

    std::fill(piece_data_[piece_index].begin(), piece_data_[piece_index].end(), 0);
}

bool PieceManager::has_piece(int piece_index) const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index)) {
        return false;
    }

    return piece_states_[piece_index] == COMPLETE;
}

bool PieceManager::is_complete() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return pieces_downloaded_ == total_pieces_;
}

PieceManager::PieceState PieceManager::get_piece_state(int piece_index) const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index)) {
        return NEEDED;
    }

    return piece_states_[piece_index];
}

std::vector<char> PieceManager::get_piece_data(int piece_index) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index) || piece_states_[piece_index] != COMPLETE) {
        return {};
    }

    return piece_data_[piece_index];
}

std::vector<char> PieceManager::get_block_data(int piece_index, int block_offset, int block_length) {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (!is_valid_piece_index(piece_index) || piece_states_[piece_index] != COMPLETE) {
        return {};
    }

    size_t piece_size = get_piece_size(piece_index);

    if (block_offset < 0 ||
        static_cast<size_t>(block_offset + block_length) > piece_size) {
        return {};
    }

    std::vector<char> block_data(block_length);
    std::copy(piece_data_[piece_index].begin() + block_offset,
              piece_data_[piece_index].begin() + block_offset + block_length,
              block_data.begin());

    return block_data;
}

double PieceManager::get_completion_percentage() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    if (total_pieces_ == 0) return 0.0;

    return (static_cast<double>(pieces_downloaded_) / static_cast<double>(total_pieces_)) * 100.0;
}

size_t PieceManager::get_bytes_downloaded() const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);
    return bytes_downloaded_;
}

size_t PieceManager::get_total_bytes() const {
    return total_bytes_;
}

std::vector<std::pair<int, int>> PieceManager::get_missing_blocks(int piece_index) const {
    std::lock_guard<std::mutex> lock(pieces_mutex_);

    std::vector<std::pair<int, int>> missing_blocks;

    if (!is_valid_piece_index(piece_index) || piece_states_[piece_index] == COMPLETE) {
        return missing_blocks;
    }

    const auto& progress = piece_progress_[piece_index];

    for (size_t block_idx = 0; block_idx < progress.blocks_needed_; ++block_idx) {
        if (!progress.blocks_received_[block_idx]) {
            int offset = static_cast<int>(block_idx * BLOCK_SIZE);
            int length = static_cast<int>(get_block_size(piece_index, block_idx));
            missing_blocks.emplace_back(offset, length);
        }
    }

    return missing_blocks;
}

void PieceManager::print_progress() const {
    double percentage = get_completion_percentage();

    std::cout << "\rprogress: " << std::fixed << std::setprecision(1)
              << percentage << "% (" << pieces_downloaded_ << "/" << total_pieces_
              << " pieces, " << bytes_downloaded_ << "/" << total_bytes_ << " bytes)";

    if (is_complete()) {
        std::cout << " - COMPLETE!" << std::endl;
    } else {
        std::cout << std::flush;
    }
}

bool PieceManager::verify_piece(int piece_index) {
    if (!is_valid_piece_index(piece_index)) {
        return false;
    }

    std::string calculated_hash = calculate_piece_hash(piece_data_[piece_index]);

    const auto& piece_hashes = torrent_->get_piece_hashes();
    const std::string& expected_hash = piece_hashes[piece_index];

    bool is_valid = (calculated_hash == expected_hash);

    if (!is_valid) {
        std::cerr << "piece " << piece_index << " hash verification failed" << std::endl;
        std::cerr << "expected: ";
        for (unsigned char c : expected_hash) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
        }
        std::cerr << std::endl << "got: ";
        for (unsigned char c : calculated_hash) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
        }
        std::cerr << std::dec << std::endl;
    }

    return is_valid;
}

bool PieceManager::write_piece_to_disk(int piece_index) {
    if (!is_valid_piece_index(piece_index) || !file_stream_) {
        return false;
    }

    size_t piece_length = torrent_->get_piece_length();
    size_t file_offset = static_cast<size_t>(piece_index) * piece_length;

    file_stream_->seekp(file_offset);

    if (!file_stream_->good()) {
        std::cerr << "failed to seek to position " << file_offset << std::endl;
        return false;
    }

    size_t piece_size = get_piece_size(piece_index);
    file_stream_->write(piece_data_[piece_index].data(), piece_size);

    if (!file_stream_->good()) {
        std::cerr << "failed to write piece " << piece_index << " to disk" << std::endl;
        return false;
    }

    file_stream_->flush();
    return true;
}

std::string PieceManager::calculate_piece_hash(const std::vector<char>& piece_data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(piece_data.data()),
         piece_data.size(), hash);

    return std::string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
}

size_t PieceManager::get_piece_size(int piece_index) const {
    if (!is_valid_piece_index(piece_index)) {
        return 0;
    }

    size_t piece_length = torrent_->get_piece_length();

    if (piece_index == static_cast<int>(total_pieces_ - 1)) {
        size_t remaining = total_bytes_ % piece_length;
        return (remaining == 0) ? piece_length : remaining;
    }

    return piece_length;
}

bool PieceManager::is_valid_piece_index(int piece_index) const {
    return piece_index >= 0 && static_cast<size_t>(piece_index) < total_pieces_;
}

size_t PieceManager::get_block_index(size_t block_offset) const {
    return block_offset / BLOCK_SIZE;
}

size_t PieceManager::get_block_size(int piece_index, size_t block_index) const {
    size_t piece_size = get_piece_size(piece_index);
    size_t block_start = block_index * BLOCK_SIZE;
    size_t remaining_in_piece = piece_size - block_start;

    return std::min(BLOCK_SIZE, remaining_in_piece);
}