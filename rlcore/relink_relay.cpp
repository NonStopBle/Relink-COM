// Standalone UDP relay daemon: forwards ReLink data frames between
// clients that registered for the same topic, for use when direct
// peer-to-peer NAT hole punching cannot cross a NAT/firewall at all
// (see relay_wire.hpp and README Step 13 for why a relay works where
// punching structurally can't -- both clients only ever open a NAT
// mapping toward this daemon's one fixed address, never toward each
// other).
//
// Performance: single recv buffer per iteration, reused for every
// forward -- a data frame is never re-encoded or copied into a second
// buffer, only sent on (sendto with the same pointer/length recvfrom
// just filled in). No heap allocation on the forwarding path: the
// per-topic member list is a fixed-capacity array reused in place, and
// membership lookups are pointer/index operations only.
//
// usage: relink-relay [port]
#include "relink/relay_wire.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace relink;

namespace {

// A member's endpoint plus when it last registered/kept alive, so
// stale entries (crashed/departed clients) get pruned instead of
// accumulating forever.
struct Member {
    struct sockaddr_in addr;
    time_t last_seen;
};

constexpr time_t kMemberTtlSeconds = 30; // must outlive the client's re-register interval

bool same_addr(const struct sockaddr_in& a, const struct sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : kRelayDefaultPort;

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }

    std::printf("relink-relay listening on 0.0.0.0:%u\n", port);
    std::fflush(stdout);

    std::unordered_map<uint32_t, std::vector<Member>> groups;
    uint8_t buf[kMaxFrameBytes];
    time_t last_sweep = std::time(nullptr);

    for (;;) {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) continue;

        uint32_t topic_id = 0;
        time_t now = std::time(nullptr);

        if (decode_relay_register(buf, static_cast<size_t>(n), &topic_id)) {
            auto& members = groups[topic_id];
            bool found = false;
            for (auto& m : members) {
                if (same_addr(m.addr, src)) { m.last_seen = now; found = true; break; }
            }
            if (!found) members.push_back(Member{src, now});
        } else if (peek_frame_topic_id(buf, static_cast<size_t>(n), &topic_id)) {
            auto it = groups.find(topic_id);
            if (it != groups.end()) {
                for (const auto& m : it->second) {
                    if (same_addr(m.addr, src)) continue; // never echo back to the sender
                    ::sendto(sock, buf, static_cast<size_t>(n), 0,
                             reinterpret_cast<const struct sockaddr*>(&m.addr), sizeof(m.addr));
                }
            }
        }
        // Anything else (malformed/unrecognized) is silently dropped --
        // a relay must never misinterpret bytes it can't identify.

        // Sweep expired members roughly once a second, not on every
        // packet -- keeps the hot path free of a per-packet time() call
        // beyond the one already taken above for last_seen bookkeeping.
        if (now != last_sweep) {
            last_sweep = now;
            for (auto& kv : groups) {
                auto& members = kv.second;
                members.erase(
                    std::remove_if(members.begin(), members.end(),
                                    [&](const Member& m) { return now - m.last_seen > kMemberTtlSeconds; }),
                    members.end());
            }
        }
    }
}
