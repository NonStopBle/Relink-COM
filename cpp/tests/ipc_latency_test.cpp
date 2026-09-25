// Same-host shared-memory IPC stress test: latency + success rate
// across many CONCURRENT TOPICS at once (the axis nat_latency_test.cpp
// covers for the network/--nat path; this is its --ipc counterpart).
// Uses ONE shared consumer RelinkNode and ONE shared producer
// RelinkNode across every topic -- the idiomatic way to use
// advertise_local_ipc/subscribe_local_ipc (one poll thread drains every
// subscribed ring in a round, see relink.hpp's ensure_shm_poll_thread())
// rather than one RelinkNode per topic, which would spin up a separate
// 200us-interval poll thread per topic and measure thread-scheduling
// contention more than the transport itself.
//
// usage: ipc_latency_test [num_topics] [messages_per_topic] [send_interval_ms]

#include "relink/relink.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <memory>

using namespace relink;

#pragma pack(push, 1)
struct LatencyMsg {
    uint64_t send_ns;
    uint32_t seq;
};
#pragma pack(pop)

struct TopicResult {
    std::vector<std::atomic<int64_t>> latency_ns; // -1 = not received
    explicit TopicResult(int n) : latency_ns(n) {
        for (auto& v : latency_ns) v = -1;
    }
};

static void send_topic(RelinkNode& producer, const std::string& topic_name,
                        int messages, int send_interval_ms) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(messages); ++i) {
        LatencyMsg m{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()), i};
        producer.publish_local_ipc(topic_name, &m, sizeof(m));
        std::this_thread::sleep_for(std::chrono::milliseconds(send_interval_ms));
    }
}

struct Stats {
    int sent = 0, received = 0;
    double min_us = 0, max_us = 0, mean_us = 0, p50_us = 0, p95_us = 0, p99_us = 0;
};

static Stats compute_stats(const std::vector<std::unique_ptr<TopicResult>>& results, int messages) {
    Stats s;
    std::vector<double> us_values;
    for (const auto& r : results) {
        s.sent += messages;
        for (auto& v : r->latency_ns) {
            int64_t ns = v.load();
            if (ns >= 0) {
                ++s.received;
                us_values.push_back(static_cast<double>(ns) / 1e3);
            }
        }
    }
    if (us_values.empty()) return s;
    std::sort(us_values.begin(), us_values.end());
    double sum = 0;
    for (double v : us_values) sum += v;
    s.min_us = us_values.front();
    s.max_us = us_values.back();
    s.mean_us = sum / us_values.size();
    s.p50_us = us_values[us_values.size() * 50 / 100];
    s.p95_us = us_values[std::min(us_values.size() - 1, us_values.size() * 95 / 100)];
    s.p99_us = us_values[std::min(us_values.size() - 1, us_values.size() * 99 / 100)];
    return s;
}

int main(int argc, char** argv) {
    int num_topics = (argc > 1) ? std::atoi(argv[1]) : 30;
    int messages_per_topic = (argc > 2) ? std::atoi(argv[2]) : 100;
    int send_interval_ms = (argc > 3) ? std::atoi(argv[3]) : 5;

    std::printf("ipc_latency_test: %d topics, %d msgs/topic, %dms interval "
                "(one shared consumer node, one shared producer node)\n",
                num_topics, messages_per_topic, send_interval_ms);

    std::vector<std::string> topic_names;
    std::vector<std::unique_ptr<TopicResult>> results;
    for (int i = 0; i < num_topics; ++i) {
        topic_names.push_back("/ipc_latency/topic_" + std::to_string(i));
        results.push_back(std::make_unique<TopicResult>(messages_per_topic));
    }

    RelinkNode consumer;
    for (int i = 0; i < num_topics; ++i) {
        TopicResult* result = results[i].get();
        consumer.subscribe_local_ipc(topic_names[i], [result](const uint8_t* payload, size_t len) {
            if (len != sizeof(LatencyMsg)) return;
            LatencyMsg m{};
            std::memcpy(&m, payload, sizeof(m));
            if (m.seq >= result->latency_ns.size()) return;
            int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            result->latency_ns[m.seq] = now_ns - static_cast<int64_t>(m.send_ns);
        });
    }

    RelinkNode producer;
    for (const auto& name : topic_names) producer.advertise_local_ipc(name);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // No per-topic warm-up publish needed here anymore: publish_local_ipc()
    // now draws shm_seq_[topic_id] under the same lock as the ring
    // lookup, so the N sender threads below racing their first publish
    // to N different topics on this shared producer node is safe (see
    // relink.hpp's publish_local_ipc()).

    std::vector<std::thread> threads;
    for (int i = 0; i < num_topics; ++i) {
        threads.emplace_back(send_topic, std::ref(producer), topic_names[i],
                              messages_per_topic, send_interval_ms);
    }
    for (auto& t : threads) t.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Stats s = compute_stats(results, messages_per_topic);
    double success_rate = s.sent > 0 ? (100.0 * s.received / s.sent) : 0.0;

    std::printf("\n=== RESULT ===\n");
    std::printf("success rate: %d/%d messages delivered (%.2f%%)\n", s.received, s.sent, success_rate);
    if (s.received > 0) {
        std::printf("latency (us): min=%.3f mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                    s.min_us, s.mean_us, s.p50_us, s.p95_us, s.p99_us, s.max_us);
    } else {
        std::printf("latency (us): n/a (no messages received)\n");
    }

    return (success_rate >= 99.0) ? 0 : 1;
}
