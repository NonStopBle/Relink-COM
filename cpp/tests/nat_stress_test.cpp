// Concurrent NAT stress test: spins up N independent pairs of nodes,
// each pair on its own topic, all registering/punching/publishing
// through the SAME relink-rlcore --nat instance at once. Reproduces
// the methodology behind the README's "30 concurrent nodes ... 900/900
// messages delivered" claim as a real, runnable, committed tool instead
// of an ad hoc one-off session.
//
// Each pair mirrors nat_punch_test.cpp's proof shape (register twice,
// punch, exchange real messages, confirm round-trip delivery) but runs
// as a thread pair inside one process instead of two separate
// processes, so N pairs can be driven and tallied from one invocation.
//
// usage: nat_stress_test <rlcore_ip> [rlcore_port] [num_pairs] [messages_per_side]

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

using namespace relink;

struct PairResult {
    bool registered_a = false;
    bool registered_b = false;
    int sent = 0;
    std::atomic<int>* received; // owned by caller
};

static void run_side(const char* label, uint32_t rlcore_ip, uint16_t rlcore_port,
                      uint32_t topic_id, int messages_per_side,
                      std::atomic<int>& received, uint8_t& registered_ok) {
    UdpTransport transport;
    transport.bind(0);

    transport.set_topic_handler(topic_id, [&](const uint8_t* payload, size_t len) {
        (void)payload; (void)len;
        ++received;
    });
    transport.start();

    uint32_t topics[1] = {topic_id};

    // transport.start() (above) already has its own recv thread holding
    // recvfrom() on this socket -- pass `&transport` so the RegisterAck
    // reply is handed off via transport->get_register_reply() instead of
    // racing that thread with a second, doomed recvfrom() here (see the
    // contract documented on register_with_rlcore_on_socket() itself).
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

    registered_ok = !peers.empty();
    if (!registered_ok) {
        transport.stop();
        return;
    }

    // Punch burst toward every learned peer candidate (public + LAN, if any).
    for (int i = 0; i < 5; ++i) {
        for (const auto& p : peers) {
            transport.publish_raw(kNatPunchTopicId, nullptr, 0, p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string msg = std::string(label) + "-payload";
    for (int i = 0; i < messages_per_side; ++i) {
        for (const auto& p : peers) {
            transport.publish_raw(topic_id, msg.data(), msg.size(), p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    transport.stop();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rlcore_ip> [rlcore_port] [num_pairs] [messages_per_side]\n", argv[0]);
        return 2;
    }
    uint32_t rlcore_ip = ipv4_to_host_order(argv[1]);
    uint16_t rlcore_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : kRlCoreDefaultPort;
    int num_pairs = (argc > 3) ? std::atoi(argv[3]) : 30;
    int messages_per_side = (argc > 4) ? std::atoi(argv[4]) : 15;

    std::printf("nat_stress_test: %d pairs, %d msgs/side, rlcore=%s:%u\n",
                num_pairs, messages_per_side, argv[1], rlcore_port);

    std::vector<std::thread> threads;
    std::vector<std::atomic<int>> received_a(num_pairs);
    std::vector<std::atomic<int>> received_b(num_pairs);
    std::vector<uint8_t> ok_a(num_pairs, 0), ok_b(num_pairs, 0);
    for (auto& r : received_a) r = 0;
    for (auto& r : received_b) r = 0;

    // Distinct topic per pair so pairs can't cross-deliver to each other.
    constexpr uint32_t kBaseTopic = 20000;

    for (int i = 0; i < num_pairs; ++i) {
        uint32_t topic = kBaseTopic + static_cast<uint32_t>(i);
        threads.emplace_back(run_side, "A", rlcore_ip, rlcore_port, topic,
                              messages_per_side, std::ref(received_b[i]), std::ref(ok_a[i]));
        threads.emplace_back(run_side, "B", rlcore_ip, rlcore_port, topic,
                              messages_per_side, std::ref(received_a[i]), std::ref(ok_b[i]));
    }
    for (auto& t : threads) t.join();

    int pairs_fully_registered = 0;
    int pairs_fully_delivered = 0;
    int total_sent = num_pairs * messages_per_side * 2;
    int total_received = 0;

    for (int i = 0; i < num_pairs; ++i) {
        bool reg_ok = ok_a[i] && ok_b[i];
        if (reg_ok) ++pairs_fully_registered;
        int ra = received_a[i].load();
        int rb = received_b[i].load();
        total_received += ra + rb;
        bool delivered_ok = (ra >= messages_per_side) && (rb >= messages_per_side);
        if (delivered_ok) ++pairs_fully_delivered;
        std::printf("pair %2d: registered=%d/%d received A<-B=%d/%d B<-A=%d/%d %s\n",
                    i, ok_a[i], ok_b[i], ra, messages_per_side, rb, messages_per_side,
                    delivered_ok ? "OK" : "FAIL");
    }

    std::printf("\n=== RESULT ===\n");
    std::printf("registration: %d/%d pairs fully registered\n", pairs_fully_registered, num_pairs);
    std::printf("delivery:     %d/%d pairs fully delivered (both directions)\n", pairs_fully_delivered, num_pairs);
    std::printf("messages:     %d/%d delivered\n", total_received, total_sent);

    return (pairs_fully_delivered == num_pairs) ? 0 : 1;
}
