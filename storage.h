#pragma once

#include "torrent_file.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

class Storage {
public:
    struct Span {
        int fd{-1};
        std::size_t length{0};
        int64_t offset{0};
    };

    Storage(const TorrentFile& torrent, const std::filesystem::path& base_path);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;

    bool write_piece(uint32_t piece_index, const std::vector<uint8_t>& data);

    std::optional<std::vector<uint8_t>> read_block(uint32_t piece_index,
                                                   uint32_t begin,
                                                   uint32_t length) const;

    std::vector<Span> spans_for(uint32_t piece_index, uint32_t begin, uint32_t length) const;

private:
    struct FileHandle {
        int fd{-1};
        std::filesystem::path path;
        int64_t length{0};
    };

    struct PieceSpan {
        uint32_t piece_index{};
        std::vector<Span> spans;
    };

    static std::filesystem::path build_path(const std::filesystem::path& base,
                                            const TorrentFile::FileEntry& entry,
                                            const std::string& root_name);

    void open_files(const std::filesystem::path& base_path);
    void build_piece_spans();

    const TorrentFile& torrent_;
    std::vector<TorrentFile::FileEntry> files_meta_;
    std::vector<FileHandle> files_;
    std::vector<PieceSpan> piece_spans_;
};


