#include "piece_manager.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

bool bitfield_test(const std::vector<uint8_t>& bf, uint32_t idx) {
    uint32_t byte = idx / 8;
    uint32_t bit = 7 - (idx % 8);
    if (byte >= bf.size()) {
        return false;
    }
    return (bf[byte] & (1u << bit)) != 0;
}

void bitfield_set(std::vector<uint8_t>& bf, uint32_t idx) {
    uint32_t byte = idx / 8;
    uint32_t bit = 7 - (idx % 8);
    if (byte >= bf.size()) {
        return;
    }
    bf[byte] |= static_cast<uint8_t>(1u << bit);
}

std::vector<uint8_t> make_bitfield(std::size_t pieces) {
    return std::vector<uint8_t>((pieces + 7) / 8, 0);
}

}

PieceManager::PieceManager(const TorrentFile& torrent, std::size_t block_size)
    : torrent_(torrent), block_size_(block_size) {
    std::size_t piece_count = torrent_.piece_hashes.size();
    pieces_.resize(piece_count);
    for (std::size_t i = 0; i < piece_count; ++i) {
        std::size_t len = piece_length_for(static_cast<uint32_t>(i));
        std::size_t blocks = (len + block_size_ - 1) / block_size_;
        pieces_[i].requested.assign(blocks, false);
        pieces_[i].blocks = blocks;
    }
    have_bitfield_ = make_bitfield(piece_count);
}

void PieceManager::set_piece_complete_callback(
    std::function<void(uint32_t, const std::vector<uint8_t>&)> cb) {
    on_complete_ = std::move(cb);
}

std::optional<PieceManager::Request> PieceManager::next_request_for_peer(
    const std::vector<uint8_t>& peer_bitfield) {
    std::size_t piece_count = pieces_.size();
    for (std::size_t offset = 0; offset < piece_count; ++offset) {
        std::size_t idx = (next_piece_cursor_ + offset) % piece_count;
        if (have_piece(static_cast<uint32_t>(idx))) {
            continue;
        }
        if (!bitfield_test(peer_bitfield, static_cast<uint32_t>(idx))) {
            continue;
        }

        PieceState& ps = pieces_[idx];
        if (!ps.buffer) {
            ps.buffer = std::make_unique<PieceBuffer>(idx, piece_length_for(idx), block_size_);
        }
        for (std::size_t b = 0; b < ps.blocks; ++b) {
            if (ps.requested[b]) {
                continue;
            }
            ps.requested[b] = true;
            uint32_t begin = static_cast<uint32_t>(b * block_size_);
            std::size_t piece_len = piece_length_for(static_cast<uint32_t>(idx));
            std::size_t remaining = piece_len - begin;
            uint32_t length =
                static_cast<uint32_t>(std::min<std::size_t>(remaining, block_size_));
            next_piece_cursor_ = (idx + 1) % piece_count;
            return Request{static_cast<uint32_t>(idx), begin, length};
        }
    }
    return std::nullopt;
}

bool PieceManager::handle_block(uint32_t piece_index,
                                uint32_t begin,
                                const std::vector<uint8_t>& data) {
    if (piece_index >= pieces_.size()) {
        return false;
    }
    if (have_piece(piece_index)) {
        return false;
    }

    PieceState& ps = pieces_[piece_index];
    if (!ps.buffer) {
        ps.buffer = std::make_unique<PieceBuffer>(piece_index, piece_length_for(piece_index),
                                                  block_size_);
    }

    auto res = ps.buffer->write_block(begin, data.data(), data.size());

    if (!res.accepted) {
        return false;
    }

    if (res.complete_now) {
        std::array<uint8_t, 20> digest{};
        SHA1(ps.buffer->data().data(), ps.buffer->data().size(), digest.data());
        if (!std::equal(digest.begin(), digest.end(), torrent_.piece_hashes[piece_index].begin())) {
            reset_piece(piece_index);
            return false;
        }
        set_have(piece_index);
        std::cout << "piece " << piece_index << " complete\n";
        if (on_complete_) {
            on_complete_(piece_index, ps.buffer->data());
        }
        ps.buffer.reset();
    }
    return true;
}

bool PieceManager::have_piece(uint32_t piece_index) const {
    if (piece_index >= pieces_.size()) {
        return false;
    }
    return pieces_[piece_index].have;
}

std::size_t PieceManager::piece_length_for(uint32_t piece_index) const {
    if (piece_index + 1 == torrent_.piece_hashes.size()) {
        int64_t full = torrent_.piece_length * static_cast<int64_t>(torrent_.piece_hashes.size() - 1);
        int64_t tail = torrent_.total_length() - full;
        return static_cast<std::size_t>(tail);
    }
    return static_cast<std::size_t>(torrent_.piece_length);
}

void PieceManager::set_have(uint32_t piece_index) {
    pieces_[piece_index].have = true;
    bitfield_set(have_bitfield_, piece_index);
}

void PieceManager::reset_piece(uint32_t piece_index) {
    if (piece_index >= pieces_.size()) {
        return;
    }
    PieceState& ps = pieces_[piece_index];
    ps.buffer.reset();
    std::fill(ps.requested.begin(), ps.requested.end(), false);
}
