#include "storage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>

static void ensure_parent_exists(const std::filesystem::path& p) {
    auto parent = p.parent_path();
    if (parent.empty()) {
        return;
    }
    std::filesystem::create_directories(parent);
}

static int open_file_rw(const std::filesystem::path& path, int64_t length) {
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return -1;
    }
    if (length > 0) {
        (void)ftruncate(fd, length);
    }
    return fd;
}

Storage::Storage(const TorrentFile& torrent, const std::filesystem::path& base_path)
    : torrent_(torrent) {
    files_meta_ = torrent_.files;
    open_files(base_path);
    build_piece_spans();
}

Storage::~Storage() {
    for (auto& f : files_) {
        if (f.fd >= 0) {
            ::close(f.fd);
        }
    }
}

bool Storage::write_piece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    if (piece_index >= piece_spans_.size()) {
        return false;
    }
    const auto& piece_span = piece_spans_[piece_index];
    std::size_t written = 0;
    for (const auto& span : piece_span.spans) {
        if (span.offset < 0 || span.length == 0) {
            return false;
        }
        if (written + span.length > data.size()) {
            return false;
        }
        ssize_t n = ::pwrite(span.fd,
                             data.data() + static_cast<std::ptrdiff_t>(written),
                             span.length,
                             span.offset);
        if (n < 0 || static_cast<std::size_t>(n) != span.length) {
            return false;
        }
        written += span.length;
    }
    return written == data.size();
}

std::optional<std::vector<uint8_t>> Storage::read_block(uint32_t piece_index,
                                                        uint32_t begin,
                                                        uint32_t length) const {
    if (piece_index >= piece_spans_.size()) {
        return std::nullopt;
    }

    if (length == 0) {
        return std::nullopt;
    }

    uint32_t piece_len = static_cast<uint32_t>(torrent_.piece_length);

    if (piece_index + 1 == torrent_.piece_hashes.size()) {
        int64_t full =
            torrent_.piece_length * static_cast<int64_t>(torrent_.piece_hashes.size() - 1);
        piece_len = static_cast<uint32_t>(torrent_.total_length() - full);
    }

    if (begin + length > piece_len) {
        return std::nullopt;
    }

    std::vector<uint8_t> out(length);
    std::size_t filled = 0;
    auto spans = spans_for(piece_index, begin, length);
    for (const auto& span : spans) {
        ssize_t n = ::pread(span.fd,
                            out.data() + static_cast<std::ptrdiff_t>(filled),
                            span.length,
                            span.offset);
        if (n < 0 || static_cast<std::size_t>(n) != span.length) {
            return std::nullopt;
        }
        filled += span.length;
    }
    return out;
}

std::vector<Storage::Span> Storage::spans_for(uint32_t piece_index,
                                              uint32_t begin,
                                              uint32_t length) const {
    std::vector<Span> result;
    if (piece_index >= piece_spans_.size()) {
        return result;
    }

    const auto& piece_span = piece_spans_[piece_index];
    uint32_t skip = begin;
    uint32_t remaining = length;

    for (const auto& span : piece_span.spans) {
        if (remaining == 0) {
            break;
        }

        if (skip >= span.length) {
            skip -= static_cast<uint32_t>(span.length);
            continue;
        }

        uint32_t available = static_cast<uint32_t>(span.length) - skip;
        uint32_t take = std::min(remaining, available);
        result.push_back(
            Span{span.fd, take, span.offset + static_cast<int64_t>(skip)});
        remaining -= take;
        skip = 0;
    }
    return result;
}

std::filesystem::path Storage::build_path(const std::filesystem::path& base,
                                          const TorrentFile::FileEntry& entry,
                                          const std::string& root_name) {
    if (entry.path.is_absolute()) {
        return entry.path;
    }
    std::filesystem::path p = base;
    p /= root_name;
    p /= entry.path;
    return p;
}

void Storage::open_files(const std::filesystem::path& base_path) {
    if (files_meta_.empty()) {
        TorrentFile::FileEntry single;
        single.length = torrent_.total_length();
        single.path = torrent_.name;
        files_meta_.push_back(single);
    }
    files_.reserve(files_meta_.size());
    for (const auto& entry : files_meta_) {
        auto full_path = build_path(base_path, entry, torrent_.name);
        ensure_parent_exists(full_path);
        int fd = open_file_rw(full_path, entry.length);
        if (fd < 0) {
            throw std::runtime_error("Failed to open " + full_path.string());
        }
        files_.push_back(FileHandle{fd, full_path, entry.length});
    }
}

void Storage::build_piece_spans() {
    std::size_t file_idx = 0;
    int64_t file_offset = 0;

    piece_spans_.resize(torrent_.piece_hashes.size());

    for (std::size_t piece = 0; piece < torrent_.piece_hashes.size(); ++piece) {
        int64_t remaining = (piece + 1 == torrent_.piece_hashes.size())
                                ? (torrent_.total_length() - static_cast<int64_t>(piece) *
                                                               torrent_.piece_length)
                                : torrent_.piece_length;
        auto& piece_span = piece_spans_[piece];

        while (remaining > 0 && file_idx < files_.size()) {
            const auto& fh = files_[file_idx];
            int64_t available = fh.length - file_offset;
            int64_t take = std::min<int64_t>(available, remaining);
            piece_span.spans.push_back(Span{fh.fd, static_cast<std::size_t>(take), file_offset});
            remaining -= take;
            file_offset += take;
            if (file_offset >= fh.length) {
                ++file_idx;
                file_offset = 0;
            }
        }
    }
}
