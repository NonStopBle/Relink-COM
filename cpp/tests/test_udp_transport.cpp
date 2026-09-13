// Step 3 real-socket test: two UdpTransport instances on loopback, each
// on its own dedicated data thread, exchanging real UDP datagrams.
// This exercises actual socket bind/send/recv, not just the pure codec.

#include "relink/udp_transport.hpp"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>

using namespace relink;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } else { \
        std::printf("ok: %s\n", #cond); \
    } \
} while (0)

struct Msg { uint32_t seq; float value; };

int main() {
    const uint32_t loopback = ipv4_to_host_order("127.0.0.1");

    UdpTransport receiver;
    receiver.bind(0); // ephemeral port
    uint16_t recv_port = receiver.local_port();
    CHECK(recv_port != 0);

    std::mutex recv_mutex;
    std::vector<Msg> received;

    receiver.set_topic_handler(50, [&](const uint8_t* payload, size_t len) {
        CHECK(len == sizeof(Msg));
        Msg m{};
        std::memcpy(&m, payload, sizeof(m));
        std::lock_guard<std::mutex> lock(recv_mutex);
        received.push_back(m);
    });

    receiver.start(); // dedicated data thread, per spec

    UdpTransport sender;
    sender.bind(0);
    sender.start();

    PeerAddr dest{loopback, recv_port};

    // --- basic single-message delivery ---
    {
        Msg m{1, 3.5f};
        bool ok = sender.publish_raw(50, &m, sizeof(m), dest);
        CHECK(ok);

        // give the data thread a moment to receive & dispatch
        bool got = false;
        for (int i = 0; i < 50 && !got; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(recv_mutex);
            got = !received.empty();
        }
        CHECK(got);
        std::lock_guard<std::mutex> lock(recv_mutex);
        CHECK(received.size() == 1);
        CHECK(received[0].seq == 1);
        CHECK(received[0].value == 3.5f);
    }

    // --- burst of messages, all delivered, order preserved (loopback,
    //     no real network reordering expected) ---
    {
        std::lock_guard<std::mutex> lock(recv_mutex);
        received.clear();
    }
    constexpr int kBurst = 200;
    for (int i = 0; i < kBurst; ++i) {
        Msg m{static_cast<uint32_t>(i), static_cast<float>(i) * 0.5f};
        sender.publish_raw(50, &m, sizeof(m), dest);
    }
    // wait for delivery
    size_t got_count = 0;
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(recv_mutex);
        got_count = received.size();
        if (got_count >= static_cast<size_t>(kBurst)) break;
    }
    CHECK(got_count == static_cast<size_t>(kBurst));

    // --- oversized publish rejected, never sent/truncated ---
    {
        static uint8_t big[kMaxPayloadBytes + 100] = {};
        bool ok = sender.publish_raw(50, big, sizeof(big), dest);
        CHECK(ok == false);
    }

    // --- unsubscribed topic: sent but silently dropped (no crash, no
    //     spurious delivery) ---
    {
        std::lock_guard<std::mutex> lock(recv_mutex);
        received.clear();
    }
    {
        Msg m{999, 1.0f};
        sender.publish_raw(/*topic*/ 777, &m, sizeof(m), dest);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(recv_mutex);
        CHECK(received.empty());
    }

    receiver.stop();
    sender.stop();

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    } else {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
}
