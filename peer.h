#pragma once
#include <string>
#include <cstdint>
#include <chrono>

class Peer {
public:
    std::string ip_;
    uint16_t port_;

    enum State {
        DISCOVERED,
        CONNECTING,
        CONNECTED,
        HANDSHAKED,
        FAILED,
        DISCONNECTED
    };

    State state;

    std::chrono::steady_clock::time_point last_seen_;
    std::chrono::steady_clock::time_point last_contact_attempt_;
    size_t connection_attempts_;
    size_t bytes_downloaded_;
    size_t bytes_uploaded_;

    std::string peer_id_;


    Peer(const std::string& ip_addr, uint16_t port_num);

    bool is_usable() const;
    bool should_retry_connection() const;
    void mark_connection_attempt();
    void mark_connection_success();
    void mark_connection_failure();

    double get_download_rate() const;
    std::chrono::seconds time_since_last_contact() const;

    bool operator==(const Peer& other) const;
    bool operator<(const Peer& other) const;

    std::string to_string() const;
    void reset_stats();

private:
    std::chrono::steady_clock::time_point connection_start_time_;
    static constexpr size_t MAX_CONNECTION_ATTEMPTS = 3;
    static constexpr std::chrono::minutes RETRY_DELAY{5};


};