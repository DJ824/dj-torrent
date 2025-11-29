#include "session.h"
#include "bencode.h"

#include <arpa/inet.h>

void Session::handle_pex(Peer& from_peer, const std::vector<uint8_t>& payload) {
    try {
        std::string s(reinterpret_cast<const char*>(payload.data()), payload.size());
        bencode::Parser parser(std::move(s));
        bencode::Value v = parser.parse();
        const auto& dict = bencode::as_dict(v);
        const auto* added_val = bencode::find_field(dict, "added");
        if (!added_val) {
            return;
        }
        const std::string& added = bencode::as_string(*added_val);
        if (added.size() % 6 != 0) {
            return;
        }
        for (std::size_t i = 0; i + 6 <= added.size(); i += 6) {
            const unsigned char* data =
                reinterpret_cast<const unsigned char*>(added.data() + i);
            char ip_str[INET_ADDRSTRLEN] = {0};
            in_addr addr{};
            std::memcpy(&addr, data, 4);
            if (!inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str))) {
                continue;
            }
            uint16_t port = static_cast<uint16_t>(data[4] << 8 | data[5]);
            PeerAddress pa;
            pa.ip = ip_str;
            pa.port = port;
            ++pex_peers_discovered_;
            add_peer(pa);
        }
    } catch (...) {
    }
}
