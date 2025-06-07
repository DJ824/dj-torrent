#pragma once
#include "bencode_parser.h"
#include "peer.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

class DHTNode {
public:
  struct NodeInfo {
    std::string node_id_;
    std::string ip_;
    uint16_t port_;
    std::chrono::steady_clock::time_point last_seen_;

    NodeInfo(const std::string &node_id, const std::string &ip, uint16_t port)
        : node_id_(node_id), ip_(ip), port_(port),
          last_seen_(std::chrono::steady_clock::now()) {}
  };
  struct DHTStats {
    size_t routing_table_size_ = 0;
    size_t total_queries_sent_ = 0;
    size_t total_responses_received_ = 0;
    size_t peers_found_ = 0;
  };

private:
  std::string node_id_;
  uint16_t port_;
  int socket_fd_;

  std::vector<NodeInfo> routing_table_;
  mutable std::mutex routing_table_mutex_;

  std::thread receive_thread_;
  std::atomic<bool> running_;
  std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes_;
  DHTStats stats_;
  mutable std::mutex stats_mutex_;

  std::function<void(std::vector<Peer>)> peer_callback_;
  std::string search_info_hash_;
  static constexpr size_t MAX_ROUTING_TABLE_SIZE = 1000;

public:
  explicit DHTNode(uint16_t listen_port = 6881);
  ~DHTNode();

  bool start();
  void stop();
  bool is_running() const { return running_; }
  void find_peers(const std::string& info_hash, std::function<void(std::vector<Peer>)> callback);
  DHTStats get_stats() const;
  const std::string& get_node_id() const { return node_id_; }
  uint16_t get_port() const { return port_; }

private:
  bool create_socket();
  void close_socket();
  void receiver_loop();
  bool send_message(const std::string& ip, uint16_t port, const std::vector<uint8_t>& message);
  std::vector<uint8_t> create_ping_query(const std::string& transaction_id);
  std::vector<uint8_t> create_find_node_query(const std::string& transaction_id, const std::string& target);
  std::vector<uint8_t> create_get_peers_query(const std::string& transaction_id, const std::string& info_hash);
  void handle_message(const std::vector<uint8_t>& data, const std::string& sender_ip, uint16_t sender_port);
  void handle_response(const BencodeValue& message, const std::string& sender_ip, uint16_t sender_port);
  void bootstrap();
  void update_routing_table(const std::string& node_id, const std::string& ip, uint16_t port);
  std::vector<NodeInfo> get_closest_nodes(const std::string& target, size_t count = 8);
  void search_for_peers(const std::string& info_hash);
  std::string generate_node_id();
  std::string generate_transaction_id();
  std::string calculate_distance(const std::string& a, const std::string& b);
  void initialize_bootstrap_nodes();
};
