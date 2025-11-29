#include "session.h"
#include "torrent_file.h"
#include "tracker_client.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        std::filesystem::path torrent_path;
        if (argc < 2) {
            torrent_path = "../data/1059680EA3988805BA59A4E2D24C7CDA4FD942DD.torrent";
        } else {
            torrent_path = argv[1];
        }
        TorrentFile torrent = TorrentFile::load(torrent_path);
        std::cout << "Loaded torrent: " << torrent.name << "\n";
        std::cout << "Pieces: " << torrent.piece_hashes.size()
                  << " piece length: " << torrent.piece_length << "\n";

        std::filesystem::path download_root = "../Downloads/";
        Session session(std::move(torrent), generate_peer_id(), 6881, 16 * 1024, download_root);
        session.start();
        session.run(500);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
