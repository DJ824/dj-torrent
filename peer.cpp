#include "peer.h"
#include <sstream>
#include <iomanip>

Peer::Peer(const std::string& ip_addr, uint16_t port_num)
    : ip_(ip_addr), port_(port_num), state(DISCOVERED),
    connection_attempts_(0), bytes_downloaded_(0), bytes_uploaded_(0) {
    auto now = std::chrono::steady_clock::now();
    last_seen_ = now;
    last_contact_attempt_ = std::chrono::steady_clock::time_point{};
    connection_start_time_ = now;
}

bool Peer::is_usable() const {
    return (state == DISCOVERED || state == CONNECTED || state == HANDSHAKED) &&
       connection_attempts_ < MAX_CONNECTION_ATTEMPTS;
}

bool Peer::should_retry_connection() const {
    if (state == CONNECTED || state == HANDSHAKED) {
        return false;
    }

    if (connection_attempts_ >= MAX_CONNECTION_ATTEMPTS) {
        return false;
    }

    if (state == DISCOVERED) {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    auto time_since_last_attempt = std::chrono::duration_cast<std::chrono::minutes>(
        now - last_contact_attempt_);
    return time_since_last_attempt >= RETRY_DELAY;
}

void Peer::mark_connection_attempt() {
    state = CONNECTING;
    last_contact_attempt_ = std::chrono::steady_clock::now();
    ++connection_attempts_;
}

void Peer::mark_connection_success() {
    state = CONNECTED;
    last_seen_ = std::chrono::steady_clock::now();
}

void Peer::mark_connection_failure() {
    state = FAILED;
    last_contact_attempt_ = std::chrono::steady_clock::now();
}

double Peer::get_download_rate() const {
    if (bytes_downloaded_ == 0.0) {
        return 0.0;
    }

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - connection_start_time_).count();

    if (duration == 0) {
        return 0.0;
    }

    return static_cast<double>(bytes_downloaded_) / duration;
}

std::chrono::seconds Peer::time_since_last_contact() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - last_seen_);
}

bool Peer::operator==(const Peer& other) const {
    return ip_ == other.ip_ && port_ == other.port_;
}

// to sort peers
bool Peer::operator<(const Peer& other) const {
    if (state != other.state) {
        // priority order HANDSHAKED > CONNECTED > DISCOVERED > others
        auto get_priority = [](State s) {
            switch (s) {
                case HANDSHAKED: return 4;
                case CONNECTED: return 3;
                case DISCOVERED: return 2;
                case CONNECTING: return 1;
                default: return 0;
            }
        };

        return get_priority(state) > get_priority(other.state);
    }

    // sort by download rate
    double my_rate = get_download_rate();
    double other_rate = other.get_download_rate();

    if (my_rate != other_rate) {
        return my_rate > other_rate;
    }

    if (ip_ != other.ip_) {
        return ip_ < other.ip_;
    }
    return port_ < other.port_;
}


std::string Peer::to_string() const {
    std::ostringstream oss;
    oss << ip << ":" << port;

    const char* state_names[] = {
        "DISCOVERED", "CONNECTING", "CONNECTED",
        "HANDSHAKED", "FAILED", "DISCONNECTED"
    };

    oss << " [" << state_names[state] << "]";

    if (bytes_downloaded_ > 0) {
        oss << " (";
        if (bytes_downloaded_ < 1024) {
            oss << bytes_downloaded_ << "B";
        } else if (bytes_downloaded_ < 1024 * 1024) {
            oss << std::fixed << std::setprecision(1)
                << (bytes_downloaded_ / 1024.0) << "KB";
        } else {
            oss << std::fixed << std::setprecision(1)
                << (bytes_downloaded_ / (1024.0 * 1024.0)) << "MB";
        }

        double rate = get_download_rate();
        if (rate > 0) {
            oss << " @ " << std::fixed << std::setprecision(1)
                << (rate / 1024.0) << "KB/s";
        }
        oss << ")";
    }

    if (connection_attempts_ > 0) {
        oss << " attempts:" << connection_attempts_;
    }

    return oss.str();
}

void Peer::reset_stats() {
    bytes_downloaded_ = 0;
    bytes_uploaded_ = 0;
    connection_attempts_ = 0;
    connection_start_time_ = std::chrono::steady_clock::now();
    last_seen_ = connection_start_time_;
}





