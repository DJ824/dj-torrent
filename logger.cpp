#include "logger.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>

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
            std::ostream& os = (rec->level == Level::Error) ? std::cerr : std::cout;
            switch (rec->level) {
                case Level::Info:
                    os << "[info] ";
                    break;
                case Level::Warn:
                    os << "[warn] ";
                    break;
                case Level::Error:
                    os << "[error] ";
                    break;
            }
            os.write(rec->msg, rec->len);
            os << std::endl;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // Drain remaining messages.
    while (auto rec = queue_.dequeue()) {
        std::ostream& os = (rec->level == Level::Error) ? std::cerr : std::cout;
        if (rec->level == Level::Info) {
            os << "[info] ";
        } else if (rec->level == Level::Warn) {
            os << "[warn] ";
        } else {
            os << "[error] ";
        }
        os.write(rec->msg, rec->len);
        os << std::endl;
    }
}
