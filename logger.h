#pragma once

#include "spsc.h"

#include <atomic>
#include <string_view>
#include <thread>

class AsyncLogger {
public:
    enum class Level { Info, Warn, Error };

    AsyncLogger();
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void start();
    void stop();

    void log(Level level, std::string_view msg);
    void info(std::string_view msg) { log(Level::Info, msg); }
    void warn(std::string_view msg) { log(Level::Warn, msg); }
    void error(std::string_view msg) { log(Level::Error, msg); }

private:
    struct Record {
        Level level;
        uint16_t len{0};
        char msg[256]{};
    };

    void run();

    LockFreeQueue<Record, 1024> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};
