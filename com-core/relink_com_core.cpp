// relink-com-core (C++ build) — small standalone registration daemon,
// per relink-com-spec.md "Mode A: relink-com-core" section.
//
// Listens on 0.0.0.0:<port> (default 8445, overridable with --port),
// receives RegisterRequest packets, updates an in-memory topic->peer
// table, and replies with RegisterAck containing the current matching
// peers for the requester's topics. Never touches the per-message data
// path -- this is bootstrap-only.

#include "relink/register.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <tuple>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace relink;

struct PeerEntry {
    uint32_t ip;
    uint16_t port;
    bool operator<(const PeerEntry& o) const {
        return std::tie(ip, port) < std::tie(o.ip, o.port);
    }
};

int main(int argc, char** argv) {
    uint16_t port = kComCoreDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
    }

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { std::perror("socket"); return 1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }

    std::printf("relink-com-core (C++) listening on 0.0.0.0:%u\n", port);

    // topic_id -> set of peers registered for it
    std::map<uint16_t, std::set<PeerEntry>> table;

    uint8_t recv_buf[2048];
    uint8_t send_buf[8192];

    while (true) {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) continue;

        DecodedRegisterRequest req{};
        if (decode_register_request(recv_buf, static_cast<size_t>(n), &req) != RegisterDecodeResult::Ok) {
            std::fprintf(stderr, "relink-com-core: dropped malformed RegisterRequest\n");
            continue;
        }

        // Update table with this node's info for each topic it declared.
        PeerEntry self{req.node_ip, req.node_port};
        for (uint16_t i = 0; i < req.topic_count; ++i) {
            uint16_t topic = register_request_topic_at(req, i);
            table[topic].insert(self);
        }

        // Build the peer list: every (ip,port,topic) for the requester's
        // topics, excluding the requester's own entry.
        std::vector<RegisterAckPeer> peers;
        for (uint16_t i = 0; i < req.topic_count; ++i) {
            uint16_t topic = register_request_topic_at(req, i);
            for (const auto& peer : table[topic]) {
                if (peer.ip == self.ip && peer.port == self.port) continue;
                peers.push_back(RegisterAckPeer{peer.ip, peer.port, topic});
            }
        }

        size_t out_len = 0;
        auto er = encode_register_ack(0, peers.data(), static_cast<uint16_t>(peers.size()),
                                       send_buf, sizeof(send_buf), &out_len);
        if (er != RegisterEncodeResult::Ok) {
            std::fprintf(stderr, "relink-com-core: failed to encode ack (too many peers?)\n");
            continue;
        }

        ::sendto(sock, send_buf, out_len, 0,
                 reinterpret_cast<struct sockaddr*>(&src), src_len);

        struct in_addr ia{};
        ia.s_addr = htonl(req.node_ip);
        std::printf("relink-com-core: registered %s:%u (%u topics), replied with %zu peers\n",
                    inet_ntoa(ia), req.node_port, req.topic_count, peers.size());
    }

    ::close(sock);
    return 0;
}
