// Large-topic-count ReLink stress test (C++) -- mirrors
// relink_py/examples/largescale_topics.py exactly, for parity testing
// between the two bindings after the same set of fixes (registration-ACK
// race, UDP recv-buffer truncation, synchronous NAT-punch-burst startup
// stall, and topic-directory MAX_ENTRIES/MAX_PACKET chunking mismatch)
// were applied to both. N topic pairs, real string-named topics (not raw
// numeric ids), default (shared) multiplex socket -- the real test of
// whether those fixes hold at realistic topic counts (e.g. ~1000).
//
// Build:
//   g++ -std=c++17 -O2 -I relink/include -pthread examples/largescale_topics.cpp -o largescale_topics_cpp
// Usage:
//   ./largescale_topics_cpp a|b <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>

#include "relink/relink.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>

struct Probe {
    uint32_t seq;
    uint64_t send_us;
    double value;
};

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string topic_name(const std::string& direction, int i, int n_pairs) {
    int width = std::to_string(n_pairs - 1).size();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/relink/stress/%s_%0*d", direction.c_str(), width, i);
    return std::string(buf);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s a|b <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>\n", argv[0]);
        return 1;
    }
    std::string role = argv[1];
    std::string rlcore_ip = argv[2];
    int n_pairs = std::atoi(argv[3]);
    double rate_hz = std::atof(argv[4]);
    double duration = std::atof(argv[5]);

    std::string out_dir = (role == "a") ? "a2b" : "b2a";
    std::string in_dir = (role == "a") ? "b2a" : "a2b";
    std::vector<std::string> out_topics, in_topics;
    for (int i = 0; i < n_pairs; ++i) {
        out_topics.push_back(topic_name(out_dir, i, n_pairs));
        in_topics.push_back(topic_name(in_dir, i, n_pairs));
    }

    relink::RelinkNode node;
    node.set_rlcore.ip(rlcore_ip);
    // Default (shared) multiplex, deliberately -- see the module comment.

    std::mutex lock;
    std::vector<int> per_topic_recv(n_pairs, 0);
    std::vector<int64_t> per_topic_max_seq(n_pairs, -1);
    std::vector<std::vector<double>> per_topic_lat(n_pairs);

    std::vector<uint32_t> in_topic_ids(n_pairs);
    for (int i = 0; i < n_pairs; ++i) {
        in_topic_ids[i] = node.topic_id_for(in_topics[i]);
    }

    for (int i = 0; i < n_pairs; ++i) {
        node.advertise<Probe>(out_topics[i]);
    }
    for (int i = 0; i < n_pairs; ++i) {
        int idx = i;
        node.subscribe<Probe>(in_topics[i], [&, idx](const Probe& m) {
            double lat_ms = (static_cast<double>(now_us()) - static_cast<double>(m.send_us)) / 1000.0;
            std::lock_guard<std::mutex> g(lock);
            per_topic_recv[idx] += 1;
            per_topic_lat[idx].push_back(lat_ms);
            if (static_cast<int64_t>(m.seq) > per_topic_max_seq[idx]) per_topic_max_seq[idx] = m.seq;
        });
    }

    std::printf("[%s] large-scale stress: %d topic pairs (%d out + %d in = %d real named topics, "
                "ONE shared socket), rate=%.1fHz/topic (aggregate %.0fHz), duration=%.1fs, rlcore=%s\n",
                role.c_str(), n_pairs, n_pairs, n_pairs, 2 * n_pairs, rate_hz, rate_hz * n_pairs,
                duration, rlcore_ip.c_str());

    double period = 1.0 / rate_hz;
    auto start = std::chrono::steady_clock::now();
    auto now_s = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };

    std::vector<uint32_t> seqs(n_pairs, 0);
    std::vector<int64_t> sent(n_pairs, 0);
    std::vector<double> next_send(n_pairs);
    for (int i = 0; i < n_pairs; ++i) next_send[i] = i * (period / std::max(1, n_pairs));

    while (now_s() < duration) {
        double t = now_s();
        double soonest = -1;
        for (int i = 0; i < n_pairs; ++i) {
            if (t >= next_send[i]) {
                Probe msg{seqs[i], now_us(), seqs[i] * 0.1};
                node.publish<Probe>(out_topics[i], msg);
                seqs[i] += 1;
                sent[i] += 1;
                next_send[i] += period;
            }
            if (soonest < 0 || next_send[i] < soonest) soonest = next_send[i];
        }
        node.spin_once();
        double remaining = soonest - now_s();
        if (remaining > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<int64_t>(std::min(remaining, 0.001) * 1e6)));
        }
    }

    auto drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < drain_until) {
        node.spin_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    int64_t total_sent = 0, total_recv = 0;
    for (int i = 0; i < n_pairs; ++i) { total_sent += sent[i]; total_recv += per_topic_recv[i]; }

    int64_t est_peer_sent = 0;
    int zero_recv_topics = 0;
    std::vector<double> all_lats;
    {
        std::lock_guard<std::mutex> g(lock);
        for (int i = 0; i < n_pairs; ++i) {
            if (per_topic_max_seq[i] >= 0) est_peer_sent += per_topic_max_seq[i] + 1;
            if (per_topic_recv[i] == 0) zero_recv_topics += 1;
            for (double v : per_topic_lat[i]) all_lats.push_back(v);
        }
    }
    std::sort(all_lats.begin(), all_lats.end());

    auto pct = [&](double p) -> double {
        if (all_lats.empty()) return -1.0;
        size_t idx = static_cast<size_t>(p * (all_lats.size() - 1));
        return all_lats[idx];
    };

    std::printf("[%s] TOTAL sent=%lld received=%lld est_peer_sent=%lld delivery=%.2f%% "
                "topics_with_zero_received=%d/%d\n",
                role.c_str(), (long long)total_sent, (long long)total_recv, (long long)est_peer_sent,
                100.0 * total_recv / std::max<int64_t>(1, est_peer_sent), zero_recv_topics, n_pairs);
    if (!all_lats.empty()) {
        std::printf("[%s] TOTAL latency_ms: p50=%.2f p90=%.2f p99=%.2f max=%.2f n=%zu\n",
                    role.c_str(), pct(0.5), pct(0.9), pct(0.99), all_lats.back(), all_lats.size());
    }

    return 0;
}
