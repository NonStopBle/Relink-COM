// relink-rlcore (C++ build) — small standalone registration daemon,
// per relink-com-spec.md "Mode A: relink-rlcore" section.
//
// Listens on 0.0.0.0:<port> (default 8445, overridable with --port),
// receives RegisterRequest packets, updates an in-memory topic->peer
// table, and replies with RegisterAck containing the current matching
// peers for the requester's topics. Never touches the per-message data
// path -- this is bootstrap-only.
//
// --nat: NAT traversal / UDP hole punching support. When set, rlcore
// substitutes each registrant's OBSERVED UDP source address (ip:port as
// seen after any NAT the registrant is behind) for the self-reported
// node_ip/node_port in the request payload, and hands THAT out to peers
// instead. A node's self-reported address is typically a private LAN IP
// that's meaningless to a peer on a different network; the observed
// source address is that node's real internet-facing, NAT-mapped
// endpoint. This alone doesn't open either side's NAT -- each node's
// RelinkNode client additionally sends a small burst of "punch" packets
// to every peer it learns about (see relink.hpp's ensure_started()),
// so both sides' NAT mappings open at roughly the same time (classic
// simultaneous-open UDP hole punching, with rlcore acting as the
// rendezvous/signaling point, same role a STUN/TURN-adjacent server
// plays elsewhere). Without --nat (the default), the self-reported
// address is used unchanged, correct for same-LAN deployments where a
// private IP is directly routable between peers.

#include "relink/register.hpp"
#include "relink/topic_directory.hpp"
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
    uint16_t port = kRlCoreDefaultPort;
    bool nat_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--nat") == 0) {
            nat_mode = true;
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

    std::printf("relink-rlcore (C++) listening on 0.0.0.0:%u%s\n", port,
                nat_mode ? " (NAT traversal enabled)" : "");

    // topic_id -> set of peers registered for it
    std::map<uint32_t, std::set<PeerEntry>> table;

    // topic_id -> name, accumulated from RLNM announces sent by any node
    // registered here (see topic_directory.hpp) -- lets rl_topic.py ask
    // rlcore for "every topic name any node in this fleet has ever
    // resolved" via a single RLNQ query, instead of having to reach each
    // node individually.
    std::map<uint32_t, std::string> topic_names;

    uint8_t recv_buf[2048];
    uint8_t send_buf[8192];

    while (true) {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) continue;

        TopicDirKind dir_kind = topic_dir_packet_kind(recv_buf, static_cast<size_t>(n));
        if (dir_kind == TopicDirKind::Announce) {
            std::vector<TopicDirEntry> entries;
            if (decode_topic_dir_entries(recv_buf, static_cast<size_t>(n), &entries)) {
                for (const auto& e : entries) topic_names[e.topic_id] = e.name;
            }
            continue;
        }
        if (dir_kind == TopicDirKind::Query) {
            std::vector<TopicDirEntry> entries;
            entries.reserve(topic_names.size());
            for (const auto& kv : topic_names) entries.push_back(TopicDirEntry{kv.first, kv.second});
            size_t reply_len = 0;
            if (encode_topic_dir_reply(entries, send_buf, sizeof(send_buf), &reply_len)) {
                ::sendto(sock, send_buf, reply_len, 0,
                         reinterpret_cast<struct sockaddr*>(&src), src_len);
            }
            continue;
        }
        if (dir_kind == TopicDirKind::Reply) continue; // rlcore never queries anyone itself

        DecodedRegisterRequest req{};
        if (decode_register_request(recv_buf, static_cast<size_t>(n), &req) != RegisterDecodeResult::Ok) {
            std::fprintf(stderr, "relink-rlcore: dropped malformed RegisterRequest\n");
            continue;
        }

        // In --nat mode, trust the observed UDP source address (the
        // real, NAT-mapped endpoint) over the self-reported one in the
        // payload, which is typically a private LAN address useless to
        // a peer outside that LAN.
        PeerEntry self;
        if (nat_mode) {
            self.ip = ntohl(src.sin_addr.s_addr);
            self.port = ntohs(src.sin_port);
        } else {
            self.ip = req.node_ip;
            self.port = req.node_port;
        }

        // Update table with this node's info for each topic it declared.
        for (uint16_t i = 0; i < req.topic_count; ++i) {
            uint32_t topic = register_request_topic_at(req, i);
            table[topic].insert(self);
        }

        // Build the peer list: every (ip,port,topic) for the requester's
        // topics, excluding the requester's own entry.
        std::vector<RegisterAckPeer> peers;
        for (uint16_t i = 0; i < req.topic_count; ++i) {
            uint32_t topic = register_request_topic_at(req, i);
            for (const auto& peer : table[topic]) {
                if (peer.ip == self.ip && peer.port == self.port) continue;
                peers.push_back(RegisterAckPeer{peer.ip, peer.port, topic});
            }
        }

        size_t out_len = 0;
        auto er = encode_register_ack(0, peers.data(), static_cast<uint16_t>(peers.size()),
                                       send_buf, sizeof(send_buf), &out_len);
        if (er != RegisterEncodeResult::Ok) {
            std::fprintf(stderr, "relink-rlcore: failed to encode ack (too many peers?)\n");
            continue;
        }

        ::sendto(sock, send_buf, out_len, 0,
                 reinterpret_cast<struct sockaddr*>(&src), src_len);

        struct in_addr ia{};
        ia.s_addr = htonl(self.ip);
        std::printf("relink-rlcore: registered %s:%u (%u topics)%s, replied with %zu peers\n",
                    inet_ntoa(ia), self.port, req.topic_count,
                    nat_mode ? " [observed]" : "", peers.size());
    }

    ::close(sock);
    return 0;
}
