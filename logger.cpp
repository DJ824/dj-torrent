#include "logger.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>


static const char* level_tag(AsyncLogger::Level level) {
    switch (level) {
        case AsyncLogger::Level::Info:
            return "info";
        case AsyncLogger::Level::Warn:
            return "warn";
        case AsyncLogger::Level::Error:
            return "error";
    }
    return "info";
}

static std::ostream& level_stream(AsyncLogger::Level level) {
    if (level == AsyncLogger::Level::Error) {
        return std::cerr;
    }
    return std::cout;
}

static void write_line(AsyncLogger::Level level, const char* msg, std::size_t len) {
    auto& os = level_stream(level);
    os << '[' << level_tag(level) << "] ";
    os.write(msg, static_cast<std::streamsize>(len));
    os.put('\n');
    os.flush();
}


AsyncLogger::AsyncLogger() = default;
AsyncLogger::~AsyncLogger() { stop(); }

void AsyncLogger::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    worker_ = std::thread([this] { run(); });
}

void AsyncLogger::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AsyncLogger::log(Level level, std::string_view msg) {
    Record rec;
    rec.level = level;
    rec.len = static_cast<uint16_t>(std::min<std::size_t>(msg.size(), sizeof(rec.msg) - 1));
    std::memcpy(rec.msg, msg.data(), rec.len);
    rec.msg[rec.len] = '\0';
    (void)queue_.enqueue(std::move(rec));
}

void AsyncLogger::run() {
    while (running_.load(std::memory_order_relaxed)) {
        if (auto rec = queue_.dequeue()) {
            write_line(rec->level, rec->msg, rec->len);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    while (auto rec = queue_.dequeue()) {
        write_line(rec->level, rec->msg, rec->len);
    }
}
