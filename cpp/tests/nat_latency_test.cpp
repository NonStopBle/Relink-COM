// Latency + success-rate stress test over relink-rlcore --nat: same
// concurrent-pairs shape as nat_stress_test.cpp (see that file for why
// pairs run as threads in one process), but each message carries a
// send timestamp so the receiver can compute one-way latency directly
// -- valid here because sender and receiver share one process/clock
// (std::chrono::steady_clock), unlike a real cross-machine deployment
// where only round-trip is measurable without clock sync.
//
// usage: nat_latency_test <rlcore_ip> [rlcore_port] [num_pairs] [messages_per_side] [send_interval_ms]

#include "relink/register.hpp"
#include "relink/rlcore_client.hpp"
#include "relink/udp_transport.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <mutex>

using namespace relink;

#pragma pack(push, 1)
struct LatencyMsg {
    uint64_t send_ns;
    uint32_t seq;
};
#pragma pack(pop)

struct SideResult {
    uint8_t registered = 0;
    std::vector<std::atomic<int64_t>> latency_ns; // -1 = not received
    SideResult(int n) : latency_ns(n) {
        for (auto& v : latency_ns) v = -1;
    }
};

static void run_side(const char* label, uint32_t rlcore_ip, uint16_t rlcore_port,
                      uint32_t topic_id, int messages_per_side, int send_interval_ms,
                      SideResult& result) {
    (void)label;
    UdpTransport transport;
    transport.bind(0);

    transport.set_topic_handler(topic_id, [&](const uint8_t* payload, size_t len) {
        if (len != sizeof(LatencyMsg)) return;
        LatencyMsg m{};
        std::memcpy(&m, payload, sizeof(m));
        if (m.seq >= result.latency_ns.size()) return;
        int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t latency = now_ns - static_cast<int64_t>(m.send_ns);
        result.latency_ns[m.seq] = latency;
    });
    transport.start();

    uint32_t topics[1] = {topic_id};
    auto outcome1 = register_with_rlcore_on_socket(
        transport.native_handle(), rlcore_ip, rlcore_port,
        0, transport.local_port(), topics, 1,
        /*max_retries=*/3, /*timeout_ms=*/500, &transport);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    auto outcome2 = register_with_rlcore_on_socket(
        transport.native_handle(), rlcore_ip, rlcore_port,
        0, transport.local_port(), topics, 1,
        /*max_retries=*/3, /*timeout_ms=*/500, &transport);

    std::vector<PeerAddr> peers;
    for (const auto& p : outcome1.peers) peers.push_back(PeerAddr{p.ip, p.port});
    for (const auto& p : outcome2.peers) {
        PeerAddr pa{p.ip, p.port};
        bool dup = false;
        for (const auto& e : peers) if (e.ip_host_order == pa.ip_host_order && e.port == pa.port) dup = true;
        if (!dup) peers.push_back(pa);
    }

    result.registered = !peers.empty();
    if (!result.registered) {
        transport.stop();
        return;
    }

    for (int i = 0; i < 5; ++i) {
        for (const auto& p : peers) transport.publish_raw(kNatPunchTopicId, nullptr, 0, p);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (uint32_t i = 0; i < static_cast<uint32_t>(messages_per_side); ++i) {
        LatencyMsg m{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()), i};
        for (const auto& p : peers) {
            transport.publish_raw(topic_id, &m, sizeof(m), p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(send_interval_ms));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    transport.stop();
}

struct Stats {
    int sent = 0, received = 0;
    double min_ms = 0, max_ms = 0, mean_ms = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0;
};

static Stats compute_stats(const std::vector<std::vector<std::atomic<int64_t>>*>& all_latencies, int messages_per_side) {
    Stats s;
    std::vector<double> ms_values;
    for (auto* vec : all_latencies) {
        s.sent += messages_per_side;
        for (auto& v : *vec) {
            int64_t ns = v.load();
            if (ns >= 0) {
                ++s.received;
                ms_values.push_back(static_cast<double>(ns) / 1e6);
            }
        }
    }
    if (ms_values.empty()) return s;
    std::sort(ms_values.begin(), ms_values.end());
    double sum = 0;
    for (double v : ms_values) sum += v;
    s.min_ms = ms_values.front();
    s.max_ms = ms_values.back();
    s.mean_ms = sum / ms_values.size();
    s.p50_ms = ms_values[ms_values.size() * 50 / 100];
    s.p95_ms = ms_values[std::min(ms_values.size() - 1, ms_values.size() * 95 / 100)];
    s.p99_ms = ms_values[std::min(ms_values.size() - 1, ms_values.size() * 99 / 100)];
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rlcore_ip> [rlcore_port] [num_pairs] [messages_per_side] [send_interval_ms]\n", argv[0]);
        return 2;
    }
    uint32_t rlcore_ip = ipv4_to_host_order(argv[1]);
    uint16_t rlcore_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : kRlCoreDefaultPort;
    int num_pairs = (argc > 3) ? std::atoi(argv[3]) : 30;
    int messages_per_side = (argc > 4) ? std::atoi(argv[4]) : 50;
    int send_interval_ms = (argc > 5) ? std::atoi(argv[5]) : 20;

    std::printf("nat_latency_test: %d pairs, %d msgs/side, %dms interval, rlcore=%s:%u\n",
                num_pairs, messages_per_side, send_interval_ms, argv[1], rlcore_port);

    constexpr uint32_t kBaseTopic = 30000;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<SideResult>> results_a, results_b;
    for (int i = 0; i < num_pairs; ++i) {
        results_a.push_back(std::make_unique<SideResult>(messages_per_side));
        results_b.push_back(std::make_unique<SideResult>(messages_per_side));
    }

    for (int i = 0; i < num_pairs; ++i) {
        uint32_t topic = kBaseTopic + static_cast<uint32_t>(i);
        threads.emplace_back(run_side, "A", rlcore_ip, rlcore_port, topic,
                              messages_per_side, send_interval_ms, std::ref(*results_a[i]));
        threads.emplace_back(run_side, "B", rlcore_ip, rlcore_port, topic,
                              messages_per_side, send_interval_ms, std::ref(*results_b[i]));
    }
    for (auto& t : threads) t.join();

    int pairs_registered = 0;
    std::vector<std::vector<std::atomic<int64_t>>*> all_latencies;
    for (int i = 0; i < num_pairs; ++i) {
        if (results_a[i]->registered && results_b[i]->registered) ++pairs_registered;
        all_latencies.push_back(&results_a[i]->latency_ns);
        all_latencies.push_back(&results_b[i]->latency_ns);
    }

    Stats s = compute_stats(all_latencies, messages_per_side);
    double success_rate = s.sent > 0 ? (100.0 * s.received / s.sent) : 0.0;

    std::printf("\n=== RESULT ===\n");
    std::printf("registration:  %d/%d pairs fully registered\n", pairs_registered, num_pairs);
    std::printf("success rate:  %d/%d messages delivered (%.2f%%)\n", s.received, s.sent, success_rate);
    if (s.received > 0) {
        std::printf("latency (ms):  min=%.3f mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                    s.min_ms, s.mean_ms, s.p50_ms, s.p95_ms, s.p99_ms, s.max_ms);
    } else {
        std::printf("latency (ms):  n/a (no messages received)\n");
    }

    return (pairs_registered == num_pairs && success_rate >= 99.0) ? 0 : 1;
}
