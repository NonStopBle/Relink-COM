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

#ifdef RELINK_ENABLE_XDP
#include "xdp/relay_xdp.hpp"
#include <csignal>
#endif

using namespace relink;

#ifdef RELINK_ENABLE_XDP
namespace {
// Global only so the signal handler (which can't take a capture) can
// reach it -- detach() is idempotent, so double-calling on a
// destructor+handler race is harmless.
relink::RelayXdp* g_xdp_for_signal = nullptr;
void handle_signal(int) {
    if (g_xdp_for_signal) g_xdp_for_signal->detach();
    std::_Exit(1);
}
} // namespace
#endif

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

// Shared handling for one received datagram, regardless of whether it
// arrived via plain recvfrom() or the AF_XDP fast path -- both loops
// below call this so REGISTER bookkeeping, forwarding, and TTL sweep
// logic exist exactly once.
void handle_packet(int sock, std::unordered_map<uint32_t, std::vector<Member>>& groups,
                    const uint8_t* buf, size_t n, const struct sockaddr_in& src,
                    time_t& last_sweep) {
    uint32_t topic_id = 0;
    time_t now = std::time(nullptr);

    if (decode_relay_register(buf, n, &topic_id)) {
        auto& members = groups[topic_id];
        bool found = false;
        for (auto& m : members) {
            if (same_addr(m.addr, src)) { m.last_seen = now; found = true; break; }
        }
        if (!found) members.push_back(Member{src, now});
    } else if (peek_frame_topic_id(buf, n, &topic_id)) {
        auto it = groups.find(topic_id);
        if (it != groups.end()) {
            for (const auto& m : it->second) {
                if (same_addr(m.addr, src)) continue; // never echo back to the sender
                ::sendto(sock, buf, n, 0,
                         reinterpret_cast<const struct sockaddr*>(&m.addr), sizeof(m.addr));
            }
        }
    }
    // Anything else (malformed/unrecognized) is silently dropped --
    // a relay must never misinterpret bytes it can't identify.

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

    std::unordered_map<uint32_t, std::vector<Member>> groups;
    time_t last_sweep = std::time(nullptr);

#ifdef RELINK_ENABLE_XDP
    // Fast path: an XDP program on the NIC redirects packets addressed
    // to `port` straight into this AF_XDP socket's UMEM, bypassing the
    // kernel's normal UDP receive path entirely (see xdp/relay_xdp.hpp
    // for why this only speeds up local processing, not the WAN RTT
    // that actually dominates relay latency). `sock` above still
    // exists and handles TX (forwarding) plus anything the XDP program
    // doesn't intercept.
    const char* ifname = (argc > 2) ? argv[2] : "eth0";
    RelayXdp xdp;
    if (xdp.init(ifname, port)) {
        g_xdp_for_signal = &xdp;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::printf("relink-relay listening on 0.0.0.0:%u (AF_XDP fast path on %s, %s mode)\n",
                    port, ifname, xdp.is_attached() ? "attached" : "unattached");
        std::fflush(stdout);

        for (;;) {
            if (!xdp.poll_rx(1000)) continue;
            uint32_t idx_rx = 0;
            uint32_t n = xdp.rx_batch(64, &idx_rx);
            if (n == 0) continue;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t* data = nullptr;
                uint32_t len = 0;
                xdp.rx_frame(idx_rx + i, &data, &len);

                // Frame data from the NIC includes Ethernet+IP+UDP
                // headers; the relay's parsing (decode_relay_register/
                // peek_frame_topic_id) expects to start at the ReLink
                // payload, so skip past them the same way the kernel
                // XDP program did (14B eth + IP header incl. options +
                // 8B UDP) to find the actual UDP payload and its
                // source address for the reply path.
                if (len > 42) {
                    const uint8_t* eth = data;
                    const uint8_t* ip = eth + 14;
                    uint8_t ihl = (ip[0] & 0x0F) * 4;
                    const uint8_t* udp = ip + ihl;
                    const uint8_t* payload = udp + 8;
                    size_t payload_len = len - (payload - data);

                    struct sockaddr_in src{};
                    src.sin_family = AF_INET;
                    std::memcpy(&src.sin_addr.s_addr, ip + 12, 4);
                    std::memcpy(&src.sin_port, udp + 0, 2);

                    handle_packet(sock, groups, payload, payload_len, src, last_sweep);
                }
            }
            xdp.release_rx(n);
        }
    }
    std::fprintf(stderr, "AF_XDP init failed, falling back to plain sockets\n");
#endif

    std::printf("relink-relay listening on 0.0.0.0:%u\n", port);
    std::fflush(stdout);

    uint8_t buf[kMaxFrameBytes];
    for (;;) {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) continue;
        handle_packet(sock, groups, buf, static_cast<size_t>(n), src, last_sweep);
    }
}
