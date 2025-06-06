#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <thread>
#include <atomic>
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <functional>
#include "torrent_file.h"
#include "bencode_parser.h"
#include "peer.h"

class TrackerClient {
public:

  struct TrackerResponse {
    std::vector<Peer> peers;
    int32_t interval = 1800;
    int32_t min_interval = 900;
    int32_t complete = 0;
    int32_t incomplete = 0;
    std::string failure_reason;
  };

private:
  TorrentFile* torrent_;
  std::string peer_id_;
  uint16_t listen_port_;

  size_t uploaded_ = 0;
  size_t downloaded_ = 0;
  size_t left_ = 0;

  std::thread announce_thread_;
  std::atomic<bool> running_{false};
  std::chrono::seconds current_interval_{1800};
  std::function<void(std::vector<Peer>)> peer_callback_;

  static SSL_CTX* ssl_ctx_;
  static bool ssl_initialized_;

public:
  explicit TrackerClient(TorrentFile* torrent, uint16_t port = 6881);
  ~TrackerClient();

  TrackerResponse announce(const std::string& event = "");
  void update_stats(size_t uploaded, size_t downloaded);

  void start_announcing(std::function<void(std::vector<Peer>)> callback);
  void stop_announcing();
  bool is_running() const;

  const std::string& get_peer_id() const;
  uint16_t get_listen_port() const;

private:
  std::string generate_peer_id();
  void calculate_stats();
  std::string build_announce_url(const std::string& event);
  std::string url_encode(const std::string& str);
  std::tuple<std::string, int, std::string> parse_url(const std::string& url);
  std::string make_http_request(const std::string &url);
  std::string make_https_request(const std::string &host, int port,
                                 const std::string &path);
  std::string build_http_request(const std::string& host, const std::string& path);
  std::string extract_http_body(const std::string& http_response);
  TrackerResponse parse_tracker_response(const std::string& response_data);
  void parse_peers_compact(const std::string& peers_data, std::vector<Peer>& peers);
  void parse_peers_list(const BencodeList& peers_list, std::vector<Peer>& peers);
  void announce_loop();
  static void initialize_ssl();
  static void cleanup_ssl();
};