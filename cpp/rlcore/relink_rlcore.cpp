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
//
// --relay: folds the standalone relink-relay daemon's PLAIN-SOCKET
// data-frame forwarding directly into this process, on this same
// socket/port -- one daemon, one port, instead of running rlcore and
// relink-relay separately. Some NAT types (notably "symmetric" NAT)
// structurally cannot be punched through no matter how the client
// retries, and a relay reachable at a single fixed address is the only
// fallback for those -- since --nat mode is exactly the case where some
// clients may have that kind of NAT, it implies --relay automatically.
// The standalone relink-relay binary still exists separately and is
// the only way to get its AF_XDP fast path (RELINK_ENABLE_XDP); this
// merged mode is plain sockets only, deliberately kept simple. On the
// client, point node.set_relay() at this SAME ip and --port (not
// relay's own default of 8446) to actually use it.

#include "relink/register.hpp"
#include "relink/topic_directory.hpp"
#include "relink/relay_wire.hpp"
#include "relink/platform.hpp"
#include "relink/crypto.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <chrono>

using namespace relink;

struct PeerEntry {
    uint32_t ip;
    uint16_t port;
    bool operator<(const PeerEntry& o) const {
        return std::tie(ip, port) < std::tie(o.ip, o.port);
    }
};

// A live node re-registers every 0.3s for as long as it's running (see
// relink.hpp's rlcore reregister interval) -- a registration this stale
// means the process exited (or died) without rlcore ever finding out,
// since there's no unregister-on-close message on this wire protocol.
// ~10x the reregister interval gives plenty of margin for a slow/missed
// tick without treating a genuinely dead node as still alive for long.
static constexpr double kRegistrationTtlSec = 3.0;

// Must outlive the client's relay re-register interval (node.py's
// _relay_keepalive_loop / relink.hpp's equivalent fires every 10s) --
// same value and rationale as the standalone relay's MEMBER_TTL_SECONDS.
static constexpr double kRelayMemberTtlSec = 30.0;

static double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--port <port>] [--ip <address>] [--nat] [--relay]\n"
        "           [--encrypt-key <64-hex>] [--generate-key] [-h|--help]\n"
        "\n"
        "  --port <port>       UDP port to listen on (default %u)\n"
        "  --ip <address>      local address to bind to (default 0.0.0.0, all interfaces)\n"
        "  --nat                enable NAT traversal / UDP hole punching (implies --relay)\n"
        "  --relay              also forward data frames between peers that can't reach\n"
        "                       each other directly, on this same port -- no separate\n"
        "                       relink-relay process needed (point the client's\n"
        "                       set_relay() at this ip:port to use it; use the\n"
        "                       standalone relink-relay instead for its AF_XDP fast path)\n"
        "  --encrypt-key <hex>  require AES-256-GCM encrypted registration (64 hex chars)\n"
        "  --generate-key       print a fresh AES-256 key and exit\n"
        "  -h, --help           show this help and exit\n",
        argv0, static_cast<unsigned>(kRlCoreDefaultPort));
}

int main(int argc, char** argv) {
    uint16_t port = kRlCoreDefaultPort;
    const char* bind_ip = nullptr;
    bool nat_mode = false;
    bool relay_mode = false;
    bool has_encrypt_key = false;
    uint8_t encrypt_key[kAesKeyBytes] = {};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--generate-key") == 0) {
            // Prints a fresh random AES-256 key and exits -- does not
            // start the daemon. Run once, then pass the printed hex to
            // both this daemon's --encrypt-key and every node's
            // node.set_rlcore.setEncryptKey(...); the same key must be
            // used on both sides for registration to succeed.
            uint8_t key[kAesKeyBytes];
            generate_random_key32(key);
            std::printf("%s\n", key32_to_hex(key).c_str());
            return 0;
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if (std::strcmp(argv[i], "--nat") == 0) {
            nat_mode = true;
        } else if (std::strcmp(argv[i], "--relay") == 0) {
            relay_mode = true;
        } else if (std::strcmp(argv[i], "--encrypt-key") == 0 && i + 1 < argc) {
            if (!hex_to_key32(argv[++i], encrypt_key)) {
                std::fprintf(stderr, "relink-rlcore: --encrypt-key expects 64 hex characters "
                             "(a 32-byte AES-256 key) -- generate one with --generate-key\n");
                return 1;
            }
            has_encrypt_key = true;
        } else {
            std::fprintf(stderr, "relink-rlcore: unrecognized argument '%s' (see --help)\n", argv[i]);
            return 1;
        }
    }
    if (nat_mode) relay_mode = true; // see the --relay comment above main()

    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) { std::perror("socket"); return 1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (bind_ip != nullptr) {
        if (::inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
            std::fprintf(stderr, "relink-rlcore: --ip '%s' is not a valid IPv4 address\n", bind_ip);
            return 1;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    addr.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        std::fprintf(stderr, "relink-rlcore: could not bind to %s:%u -- another process may "
                     "already be listening there (try --port <other-port>, or check `ss -ulnp`)\n",
                     bind_ip != nullptr ? bind_ip : "0.0.0.0", port);
        return 1;
    }

    {
        std::string status;
        if (nat_mode) status += "NAT traversal enabled";
        if (relay_mode) {
            if (!status.empty()) status += ", ";
            status += "relay forwarding enabled";
            if (nat_mode) status += " (auto via --nat)";
        }
        std::string suffix = status.empty() ? std::string() : (" (" + status + ")");
        std::printf("relink-rlcore (C++) listening on %s:%u%s\n",
                    bind_ip != nullptr ? bind_ip : "0.0.0.0", port, suffix.c_str());
    }

    // topic_id -> set of peers registered for it
    std::map<uint32_t, std::set<PeerEntry>> table;

    // (topic_id, peer) -> time of its most recent RegisterRequest --
    // lets prune_stale() below evict a registration once its owning node
    // stops re-registering (closed/crashed), so `rl_topic.py list`/
    // `info` and peer discovery both stop treating a dead node's topics
    // as live. See kRegistrationTtlSec.
    std::map<std::pair<uint32_t, PeerEntry>, double> last_seen;

    // (topic_id, peer) -> role bitmask (kRolePublisher | kRoleSubscriber),
    // from periodic RLPA role announces -- lets `rl_topic.py info` show
    // who's publishing vs subscribing a topic. Pruned by the same TTL as
    // registrations, via its own last-seen map, since role announces are
    // sent on the same periodic cadence as reregistration.
    std::map<std::pair<uint32_t, PeerEntry>, uint8_t> role_table;
    std::map<std::pair<uint32_t, PeerEntry>, double> role_last_seen;

    // topic_id -> {peer -> time of last REGISTER/keepalive} -- only
    // populated/consulted when relay_mode, per-topic forwarding group
    // membership for the merged relay path (was the standalone relay's
    // `groups`, now living here). Distinct from `table` above: rlcore
    // registrations there track for peer-discovery/RegisterAck purposes,
    // not who wants relayed copies of a topic's data frames.
    std::map<uint32_t, std::map<PeerEntry, double>> relay_groups;

    auto prune_stale = [&]() {
        double now = now_sec();
        for (auto it = table.begin(); it != table.end(); ) {
            auto& peers = it->second;
            for (auto pit = peers.begin(); pit != peers.end(); ) {
                auto ls = last_seen.find({it->first, *pit});
                double age = (ls != last_seen.end()) ? (now - ls->second) : kRegistrationTtlSec + 1;
                if (age > kRegistrationTtlSec) {
                    if (ls != last_seen.end()) last_seen.erase(ls);
                    pit = peers.erase(pit);
                } else {
                    ++pit;
                }
            }
            if (peers.empty()) it = table.erase(it);
            else ++it;
        }
        for (auto it = role_last_seen.begin(); it != role_last_seen.end(); ) {
            if (now - it->second > kRegistrationTtlSec) {
                role_table.erase(it->first);
                it = role_last_seen.erase(it);
            } else {
                ++it;
            }
        }
        if (relay_mode) {
            for (auto it = relay_groups.begin(); it != relay_groups.end(); ) {
                auto& members = it->second;
                for (auto mit = members.begin(); mit != members.end(); ) {
                    if (now - mit->second > kRelayMemberTtlSec) mit = members.erase(mit);
                    else ++mit;
                }
                if (members.empty()) it = relay_groups.erase(it);
                else ++it;
            }
        }
    };

    // Wake up periodically even with no incoming traffic, purely to run
    // prune_stale() -- otherwise a fleet that goes quiet keeps every last
    // registration "alive" forever, since nothing else ever calls it.
    set_recv_timeout_ms(sock, 1000);

    // topic_id -> name, accumulated from RLNM announces sent by any node
    // registered here (see topic_directory.hpp) -- lets rl_topic.py ask
    // rlcore for "every topic name any node in this fleet has ever
    // resolved" via a single RLNQ query, instead of having to reach each
    // node individually.
    std::map<uint32_t, std::string> topic_names;

    // 65507 (largest possible UDP/IPv4 datagram), not a smaller fixed
    // size: a RegisterRequest with many topics or an RLNM announce with
    // real topic names can legitimately exceed a couple KB, and
    // recvfrom()'s length argument silently truncates anything past it
    // at the kernel level for UDP -- no error, just corrupted data that
    // decode_register_request()/decode_topic_dir_entries() then
    // correctly reject as malformed, even though the sender sent a
    // perfectly valid packet. Measured: 500 real topic names produced a
    // 13506-byte announce, well past the original 2048-byte recv_buf.
    uint8_t recv_buf[65507];
    uint8_t send_buf[65507];

    while (true) {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = static_cast<ssize_t>(::recvfrom(sock,
                                reinterpret_cast<char*>(recv_buf), static_cast<int>(sizeof(recv_buf)), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len));
        prune_stale();
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
            // Include every topic id `table` currently has a LIVE
            // registration for (prune_stale() already dropped anything
            // past kRegistrationTtlSec), not just ones that got a name
            // via RLNM -- a topic advertised/subscribed with a raw
            // numeric id (no string name) is real and actively routed,
            // but would otherwise be completely invisible to
            // `rl_topic.py list`. Such ids are sent with an empty name
            // (wire format already supports name_len=0); topic_names
            // still wins for anything named. Deliberately NOT unioned
            // with topic_names' own keys: a name whose topic has no live
            // registrant left (the node that announced it exited) must
            // stop being listed too -- that's the whole point of this
            // bug fix.
            std::vector<TopicDirEntry> entries;
            entries.reserve(table.size());
            for (const auto& kv : table) {
                auto it = topic_names.find(kv.first);
                entries.push_back(TopicDirEntry{kv.first, it != topic_names.end() ? it->second : std::string()});
            }
            // chunk_topic_dir_entries(), not one encode_topic_dir_reply()
            // call: at real large-topic-count scale (e.g. ~1000 topics
            // across a fleet) topic_names can need multiple RLNR reply
            // packets -- see that function's doc comment. rl_topic's
            // query_rlcore() collects every chunk sent here. An empty
            // topic_names still sends one empty reply, matching the
            // original single-packet behavior.
            auto chunks = chunk_topic_dir_entries(entries);
            if (chunks.empty()) chunks.emplace_back();
            for (const auto& chunk : chunks) {
                size_t reply_len = 0;
                if (encode_topic_dir_reply(chunk, send_buf, sizeof(send_buf), &reply_len)) {
                    ::sendto(sock, reinterpret_cast<const char*>(send_buf), static_cast<int>(reply_len), 0,
                             reinterpret_cast<struct sockaddr*>(&src), src_len);
                }
            }
            continue;
        }
        if (dir_kind == TopicDirKind::Reply) continue; // rlcore never queries anyone itself

        RoleDirKind role_kind = role_packet_kind(recv_buf, static_cast<size_t>(n));
        if (role_kind == RoleDirKind::Announce) {
            // Always the OBSERVED UDP source, regardless of --nat --
            // this is purely diagnostic ("who is really talking to me on
            // this topic"), and the observed source is always the true
            // endpoint for that purpose, unlike node_ip/node_port in a
            // RegisterRequest payload which self-reports a possibly
            // private/unroutable address.
            std::vector<RoleAnnounceEntry> entries;
            if (decode_role_announce(recv_buf, static_cast<size_t>(n), &entries)) {
                PeerEntry observed{ntohl(src.sin_addr.s_addr), ntohs(src.sin_port)};
                double reg_now = now_sec();
                for (const auto& e : entries) {
                    auto key = std::make_pair(e.topic_id, observed);
                    role_table[key] = e.role;
                    role_last_seen[key] = reg_now;
                }
            }
            continue;
        }
        if (role_kind == RoleDirKind::Query) {
            std::vector<RolePeerEntry> entries;
            entries.reserve(role_table.size());
            for (const auto& kv : role_table) {
                entries.push_back(RolePeerEntry{kv.first.first, kv.second, kv.first.second.ip, kv.first.second.port});
            }
            auto chunks = chunk_role_peer_entries(entries);
            if (chunks.empty()) chunks.emplace_back();
            for (const auto& chunk : chunks) {
                size_t reply_len = 0;
                if (encode_role_reply(chunk, send_buf, sizeof(send_buf), &reply_len)) {
                    ::sendto(sock, reinterpret_cast<const char*>(send_buf), static_cast<int>(reply_len), 0,
                             reinterpret_cast<struct sockaddr*>(&src), src_len);
                }
            }
            continue;
        }
        if (role_kind == RoleDirKind::Reply) continue; // rlcore never queries anyone itself

        if (relay_mode) {
            // A relay REGISTER control packet (join a topic's forwarding
            // group) or an ordinary data frame to forward -- checked
            // before the RegisterRequest path below since neither shape
            // can ever be a valid (plaintext or encrypted) RegisterRequest
            // (see decode_relay_register/peek_frame_topic_id's exact
            // size/start-byte checks), so this never steals traffic that
            // path would otherwise have handled.
            uint32_t relay_topic_id = 0;
            if (decode_relay_register(recv_buf, static_cast<size_t>(n), &relay_topic_id)) {
                PeerEntry from{ntohl(src.sin_addr.s_addr), ntohs(src.sin_port)};
                relay_groups[relay_topic_id][from] = now_sec();
                continue;
            }
            uint32_t frame_topic_id = 0;
            if (peek_frame_topic_id(recv_buf, static_cast<size_t>(n), &frame_topic_id)) {
                auto it = relay_groups.find(frame_topic_id);
                if (it != relay_groups.end()) {
                    PeerEntry from{ntohl(src.sin_addr.s_addr), ntohs(src.sin_port)};
                    for (const auto& kv : it->second) {
                        const PeerEntry& member = kv.first;
                        if (member.ip == from.ip && member.port == from.port) continue; // never echo back to the sender
                        struct sockaddr_in dst{};
                        dst.sin_family = AF_INET;
                        dst.sin_addr.s_addr = htonl(member.ip);
                        dst.sin_port = htons(member.port);
                        ::sendto(sock, reinterpret_cast<const char*>(recv_buf), static_cast<int>(n), 0,
                                 reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
                    }
                }
                continue;
            }
        }

        // When --encrypt-key is set, every RegisterRequest must be an
        // AES-256-GCM-sealed blob under that key -- opened here before
        // decoding. A plaintext or wrong-key request fails to open and
        // is dropped the same way a malformed one always was; this
        // also authenticates the sender (GCM's tag), not just hides the
        // payload from onlookers.
        const uint8_t* req_data = recv_buf;
        size_t req_data_len = static_cast<size_t>(n);
        uint8_t decrypted_req[65507];
        if (has_encrypt_key) {
            size_t decrypted_len = 0;
            if (!aes256gcm_open(encrypt_key, recv_buf, static_cast<size_t>(n),
                                 decrypted_req, sizeof(decrypted_req), &decrypted_len)) {
                std::fprintf(stderr, "relink-rlcore: dropped RegisterRequest that failed to "
                             "decrypt (missing/wrong key on the sending node?)\n");
                continue;
            }
            req_data = decrypted_req;
            req_data_len = decrypted_len;
        }

        DecodedRegisterRequest req{};
        if (decode_register_request(req_data, req_data_len, &req) != RegisterDecodeResult::Ok) {
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
        double reg_now = now_sec();
        for (uint16_t i = 0; i < req.topic_count; ++i) {
            uint32_t topic = register_request_topic_at(req, i);
            table[topic].insert(self);
            last_seen[{topic, self}] = reg_now;
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

        const uint8_t* wire_ack = send_buf;
        size_t wire_ack_len = out_len;
        uint8_t sealed_ack[65507];
        if (has_encrypt_key) {
            size_t sealed_len = 0;
            if (!aes256gcm_seal(encrypt_key, send_buf, out_len, sealed_ack, sizeof(sealed_ack), &sealed_len)) {
                std::fprintf(stderr, "relink-rlcore: failed to encrypt RegisterAck, not replying\n");
                continue;
            }
            wire_ack = sealed_ack;
            wire_ack_len = sealed_len;
        }

        ::sendto(sock, reinterpret_cast<const char*>(wire_ack), static_cast<int>(wire_ack_len), 0,
                 reinterpret_cast<struct sockaddr*>(&src), src_len);

        struct in_addr ia{};
        ia.s_addr = htonl(self.ip);
        std::printf("relink-rlcore: registered %s:%u (%u topics)%s, replied with %zu peers\n",
                    inet_ntoa(ia), self.port, req.topic_count,
                    nat_mode ? " [observed]" : "", peers.size());
    }

    relink::close_socket(sock);
    return 0;
}
