// ReLink-com performance benchmark — measures whether the 1000Hz / <=1ms
// target from relink-com-spec.md's "Performance target" section is met.
//
// This is USAGE code exercising the future ReLink API, same as
// relink_example.cpp — not the library implementation itself.
//
// Run as two processes: bench pub  |  bench sub
// The subscriber records one-way latency for every message received and
// prints p50/p99/worst-case at the end — matching the spec's explicit
// requirement to measure the full distribution, not just an average,
// since a single missed deadline at 1000Hz matters.

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>
#include <csignal>

#pragma pack(push, 1)
struct BenchMsg {
    uint64_t send_time_us;  // sender's timestamp, used to compute one-way latency
    uint8_t  padding[32];   // pad to a realistic small-message size (~40 bytes),
                             // per the spec's note: benchmark with a realistic
                             // payload size, not a trivial 1-byte message
};
#pragma pack(pop)

enum BenchTopics : uint16_t { TOPIC_BENCH = 200 };

static uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

// --- publisher: sends at a sustained 1000Hz for a fixed duration ---
static void run_publisher(RelinkNode& node) {
    node.advertise<BenchMsg>(TOPIC_BENCH);

    constexpr int DURATION_SEC = 60;   // spec step 8: sustained duration,
                                         // not just a short burst
    constexpr int RATE_HZ = 1000;
    const auto period = std::chrono::microseconds(1000000 / RATE_HZ);

    std::printf("publisher: sending at %d Hz for %d seconds...\n",
                RATE_HZ, DURATION_SEC);

    auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    uint64_t sent = 0;

    while (!g_stop) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(DURATION_SEC)) break;

        BenchMsg msg{};
        msg.send_time_us = now_us();
        node.publish<BenchMsg>(TOPIC_BENCH, msg);
        ++sent;

        node.spin_once();

        // Fixed-rate pacing: sleep until the next scheduled send time
        // rather than a flat sleep(1ms), so publisher jitter doesn't
        // itself skew the subscriber-side latency measurement.
        next_send += period;
        std::this_thread::sleep_until(next_send);
    }

    std::printf("publisher: done, sent %llu messages\n",
                (unsigned long long)sent);
}

// --- subscriber: records one-way latency for every message received ---
static void run_subscriber(RelinkNode& node) {
    std::vector<double> latencies_us;
    latencies_us.reserve(70000);  // ~60s at 1000Hz plus margin

    uint64_t first_recv_time = 0;
    uint64_t last_recv_time = 0;

    node.subscribe<BenchMsg>(TOPIC_BENCH, [&](const BenchMsg& msg) {
        uint64_t recv_time = now_us();
        // NOTE: this assumes clocks are reasonably synced between the two
        // machines (e.g. both NTP-synced) — on a single machine or with
        // synced clocks this is a valid one-way latency measurement; if
        // clocks drift, treat these numbers as relative/round-trip
        // instead and adjust the harness accordingly.
        double latency_us = double(recv_time - msg.send_time_us);
        latencies_us.push_back(latency_us);
        if (first_recv_time == 0) first_recv_time = recv_time;
        last_recv_time = recv_time;
    });

    std::printf("subscriber: listening on TOPIC_BENCH, press Ctrl+C when "
                "the publisher finishes...\n");

    while (!g_stop) {
        node.spin_once();
    }

    if (latencies_us.empty()) {
        std::printf("subscriber: no messages received\n");
        return;
    }

    std::sort(latencies_us.begin(), latencies_us.end());
    size_t n = latencies_us.size();
    double p50    = latencies_us[n * 50 / 100];
    double p99    = latencies_us[n * 99 / 100];
    double worst  = latencies_us[n - 1];
    double best   = latencies_us[0];
    double sum = 0;
    for (double v : latencies_us) sum += v;
    double avg = sum / n;

    std::printf("\n--- results (%zu messages) ---\n", n);
    std::printf("best:   %8.1f us\n", best);
    std::printf("avg:    %8.1f us\n", avg);
    std::printf("p50:    %8.1f us\n", p50);
    std::printf("p99:    %8.1f us\n", p99);
    std::printf("worst:  %8.1f us\n", worst);

    // spec's hard requirement: <=1000us (1ms) per message, sustained.
    // Judge against p99/worst, not avg — an average well under budget
    // can still hide a tail that occasionally blows it.
    constexpr double BUDGET_US = 1000.0;
    if (worst <= BUDGET_US) {
        std::printf("\nPASS: worst-case %.1fus is within the %.0fus budget\n",
                    worst, BUDGET_US);
    } else {
        std::printf("\nFAIL: worst-case %.1fus exceeds the %.0fus budget "
                    "-- profile against the Lean optimization checklist "
                    "(allocation, locking, blocking recv) before assuming "
                    "the architecture itself needs a rewrite\n",
                    worst, BUDGET_US);
    }

    // Actual received rate: message count divided by the wall-clock span
    // between the first and last receive, NOT 1e6/avg_latency -- that
    // inverts one-way latency and calls it a rate, which is a different
    // quantity entirely (it grows the *lower* latency gets, and would
    // report a huge bogus number for a single very-fast message). The
    // publisher paces at a fixed 1000Hz regardless of latency; this
    // number is how close the subscriber's actual delivery rate came to
    // that, not a measure of how "fast" the system is.
    double span_sec = double(last_recv_time - first_recv_time) / 1'000'000.0;
    if (span_sec > 0.0 && n > 1) {
        double received_hz = double(n - 1) / span_sec;
        std::printf("received rate: ~%.0f Hz (%zu messages over %.1fs)\n",
                    received_hz, n, span_sec);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub]\n", argv[0]);
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    RelinkNode node;
    node.set_com_core.ip("10.0.0.5");  // port defaults to 8445

    if (std::strcmp(argv[1], "pub") == 0) {
        run_publisher(node);
    } else {
        run_subscriber(node);
    }
    return 0;
}
