// Step 5 real-socket test: two MulticastDiscovery instances (loopback
// multicast, IP_MULTICAST_LOOP) exchanging real beacon packets.
// Confirms: overlap -> peer stored, no overlap -> discarded, no self-
// peering, and that the outcome matches what mode A (rlcore) already
// proved in test step 4 for the same two-node/topic scenario.

#include "relink/multicast_discovery.hpp"
#include "relink/udp_transport.hpp" // ipv4_to_host_order
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    const uint32_t loopback = ipv4_to_host_order("127.0.0.1");

    // Node A: publishes/subscribes topic 100 and 101 (mirrors the
    // rlcore interop test's node at port 9001, topics 100/101).
    MulticastDiscoveryConfig cfg_a;
    cfg_a.self_ip = loopback;
    cfg_a.port_groups = {{9001, {100, 101}}};
    cfg_a.startup_burst_count = 3;
    cfg_a.startup_jitter_max_ms = 20; // fast for test
    cfg_a.reannounce_min_ms = 300;
    cfg_a.reannounce_max_ms = 500;

    // Node B: topics 100 and 200 (mirrors rlcore test's node at
    // port 9002) -- topic 100 overlaps with A, topic 200/101 do not.
    MulticastDiscoveryConfig cfg_b;
    cfg_b.self_ip = loopback;
    cfg_b.port_groups = {{9002, {100, 200}}};
    cfg_b.startup_burst_count = 3;
    cfg_b.startup_jitter_max_ms = 20;
    cfg_b.reannounce_min_ms = 300;
    cfg_b.reannounce_max_ms = 500;

    MulticastDiscovery node_a(cfg_a);
    MulticastDiscovery node_b(cfg_b);

    std::mutex log_mutex;
    std::vector<std::pair<uint16_t, PeerInfo>> a_discoveries, b_discoveries;

    node_a.set_peer_discovered_callback([&](uint16_t topic, const PeerInfo& p) {
        std::lock_guard<std::mutex> lock(log_mutex);
        a_discoveries.push_back({topic, p});
    });
    node_b.set_peer_discovered_callback([&](uint16_t topic, const PeerInfo& p) {
        std::lock_guard<std::mutex> lock(log_mutex);
        b_discoveries.push_back({topic, p});
    });

    node_a.start();
    node_b.start();

    // Give the startup burst + a little slack time to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // --- Node A must have discovered B only on topic 100 (the overlap),
    //     never on 200 (A doesn't declare 200) ---
    {
        auto peers_100 = node_a.peers_for_topic(100);
        CHECK(peers_100.size() == 1);
        if (!peers_100.empty()) {
            CHECK(peers_100[0].ip == loopback);
            CHECK(peers_100[0].port == 9002);
        }
        auto peers_101 = node_a.peers_for_topic(101);
        CHECK(peers_101.empty()); // B never declared 101
        auto peers_200 = node_a.peers_for_topic(200); // A never declared 200 at all
        CHECK(peers_200.empty());
    }

    // --- Node B must have discovered A only on topic 100 ---
    {
        auto peers_100 = node_b.peers_for_topic(100);
        CHECK(peers_100.size() == 1);
        if (!peers_100.empty()) {
            CHECK(peers_100[0].ip == loopback);
            CHECK(peers_100[0].port == 9001);
        }
        auto peers_200 = node_b.peers_for_topic(200); // A doesn't have 200
        CHECK(peers_200.empty());
    }

    // --- known_topic_count: only topics with at least one discovered
    //     peer are tracked -- both nodes should show exactly 1 (topic 100) ---
    CHECK(node_a.known_topic_count() == 1);
    CHECK(node_b.known_topic_count() == 1);

    // --- discovery callback fired exactly once per topic overlap (no
    //     duplicate spam from repeated beacons within this window,
    //     since insert() only fires the callback for genuinely new peers) ---
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        CHECK(a_discoveries.size() == 1);
        CHECK(a_discoveries[0].first == 100);
        CHECK(b_discoveries.size() == 1);
        CHECK(b_discoveries[0].first == 100);
    }

    // --- sparse re-announce: wait past one reannounce interval and
    //     confirm the peer table is still correct (no duplicate/garbage
    //     entries introduced by repeated beacons) ---
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(node_a.peers_for_topic(100).size() == 1);
    CHECK(node_b.peers_for_topic(100).size() == 1);

    node_a.stop();
    node_b.stop();

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
