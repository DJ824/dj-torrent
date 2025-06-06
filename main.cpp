#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>
#include <random>
#include <atomic>
#include <iomanip>
#include "peer_manager.h"
#include "tracker.h"
#include "peer_connection.h"

std::atomic<bool> shutdown_requested(false);

void signal_handler(int signal) {
  std::cout << "\nShutdown requested (signal " << signal << ")..." << std::endl;
  shutdown_requested = true;
}

void print_usage(const char* program_name) {
  std::cout << "usage: " << program_name << " <torrent_file> [download_directory]" << std::endl;
  std::cout << "example: " << program_name << " ubuntu.torrent ./downloads/" << std::endl;
}

std::string generate_peer_id() {
  std::string peer_id = "-DJ0001-";
  const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

  for (int i = 0; i < 12; ++i) {
    peer_id += charset[dis(gen)];
  }
  return peer_id;
}

void print_status_line(const PieceManager& piece_manager, const PeerManager& peer_manager) {
  auto stats = peer_manager.get_stats();
  double completion = piece_manager.get_completion_percentage();

  std::cout << "\r[DJ-Torrent] Progress: " << std::fixed << std::setprecision(1)
            << completion << "% | Peers: " << stats.active_connections
            << " active | Rate: " << std::setprecision(2)
            << (stats.download_rate / 1024.0) << " KB/s | Downloaded: "
            << std::setprecision(1) << (piece_manager.get_bytes_downloaded() / (1024.0 * 1024.0))
            << " MB" << std::flush;
}

int main(int argc, char* argv[]) {


  std::string torrent_file = "ubuntu-25.04-desktop-amd64.iso.torrent";
  std::string download_dir = (argc >=3) ? argv[2] : "./downloads/";
  std::cout << "torrent file: " << torrent_file << std::endl;
  std::cout << "download directory: " << download_dir << std::endl;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  try {
    std::cout << "parsing torrent file... " << std::endl;
    TorrentFile torrent;
    if (!torrent.parse(torrent_file)) {
      std::cerr << "faield to parse torrent file" << std::endl;
      return 1;
    }

    torrent.print_info();

    std::cout << "initializing piece manager" << std::endl;
    PieceManager piece_manager(&torrent, download_dir);
    if (piece_manager.is_complete()) {
      std::cout << "download already complete" << std::endl;
      return 0;
    }

    std::string peer_id = generate_peer_id();
    std::cout << "using peer id: " << peer_id.substr(0,8) << "..." << std::endl;
    std::cout << "initializing peer manager" << std::endl;
    PeerManager peer_manager(&piece_manager, &torrent, peer_id);

    std::cout << "initializing tracker client" << std::endl;
    TrackerClient tracker_client(&torrent);
    tracker_client.start_announcing([&peer_manager](std::vector<Peer> peers) {
       peer_manager.add_peers(peers);
    });

    peer_manager.start();

    auto last_status_update = std::chrono::steady_clock::now();
    auto last_progress_print = std::chrono::steady_clock::now();

    while (!piece_manager.is_complete() && !shutdown_requested) {
      auto now = std::chrono::steady_clock::now();

      if (now - last_status_update >= std::chrono::seconds(1)) {
        print_status_line(piece_manager, peer_manager);
        last_status_update = now;
      }

      if (now - last_progress_print >= std::chrono::seconds(30)) {
        std::cout << std::endl;
        piece_manager.print_progress();
        auto stats = peer_manager.get_stats();
        std::cout << "peer stats - active: " << stats.active_connections
        << ", total tried: " << stats.total_peers_tried
        << ", failed: " << stats.failed_connections << std::endl;
        last_progress_print = now;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (piece_manager.is_complete()) {
      std::cout << "download complete " << std::endl;
      piece_manager.print_progress();
    } else {
      std::cout << "download interrupted by user" << std::endl;
    }

    std::cout << "shutting down" << std::endl;
    peer_manager.stop();
    tracker_client.stop_announcing();
    auto stats = peer_manager.get_stats();
    std::cout << "final stats:" << std::endl;
    std::cout << "total peers tried: " << stats.total_peers_tried << std::endl;
    std::cout << "failed connections: " << stats.failed_connections << std::endl;
    std::cout << "final download rate: " << std::fixed << std::setprecision(2)
              << (stats.download_rate / 1024.0) << "kb/s" << std::endl;
    std::cout << "bytes downloaded: " << piece_manager.get_bytes_downloaded() << " / "
              << piece_manager.get_total_bytes();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
}