#pragma once
#include "piece_buffer.h"
#include "torrent_file.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

class PieceManager {
public:
    struct Request {
        uint32_t piece_index;
        uint32_t begin;
        uint32_t length;
    };

    explicit PieceManager(const TorrentFile& torrent, std::size_t block_size);

    void set_piece_complete_callback(
        std::function<void(uint32_t, const std::vector<uint8_t>&)> cb);

    std::optional<Request> next_request_for_peer(const std::vector<uint8_t>& peer_bitfield);
    std::optional<Request> next_request_for_peer_rarest(const std::vector<uint8_t>& peer_bitfield);
    bool handle_block(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& data);
    const std::vector<uint8_t>& have_bitfield() const { return have_bitfield_; }
    bool have_piece(uint32_t piece_index) const;
    std::vector<uint32_t> sum_peer_bitfield_ct_;
    void update_buckets();


private:
    struct PieceState {
        bool have{false};
        std::vector<bool> requested;
        std::unique_ptr<PieceBuffer> buffer;
        std::size_t blocks{0};
    };
    std::vector<std::vector<size_t>> piece_buckets_;
    std::optional<std::size_t> lowest_nonempty_bucket() const;
    std::size_t piece_length_for(uint32_t piece_index) const;
    void set_have(uint32_t piece_index);
    void reset_piece(uint32_t piece_index);
    uint64_t piece_ct_ = 0;
    const TorrentFile& torrent_;
    std::size_t block_size_;
    std::vector<PieceState> pieces_;
    std::vector<uint8_t> have_bitfield_;
    std::function<void(uint32_t, const std::vector<uint8_t>&)> on_complete_;
    std::size_t next_piece_cursor_{0};
    void rarest_first();
};
