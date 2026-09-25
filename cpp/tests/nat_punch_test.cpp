// Real cross-machine NAT hole-punching test. Unlike RelinkNode's normal
// one-shot registration, this test registers TWICE (with a pause in
// between) so BOTH sides have a chance to learn about each other via
// rlcore's one-shot ack (rlcore doesn't push updates to earlier
// registrants) -- this mirrors what a periodic re-registration policy
// would give you in production, deliberately done manually here to keep
// RelinkNode itself unchanged.
//
// Both sides then send a punch burst (opens each side's own NAT mapping
// toward the other), then exchange a few real messages in BOTH
// directions and confirm round-trip delivery. If NAT hole punching
// didn't actually work, the inbound side's own NAT would silently drop
// the other side's real data too -- so a successful round trip is a
// genuine proof, not just a registration-level check.
//
// usage: nat_punch_test <a|b> <rlcore_public_ip> [rlcore_port]

#include "relink/register.hpp"
#include "relink/rlcore_client.hpp"
#include "relink/udp_transport.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>

using namespace relink;

enum : uint16_t { TOPIC_PUNCH_TEST = 999 };

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <a|b> <rlcore_public_ip> [rlcore_port]\n", argv[0]);
        return 2;
    }
    std::string role = argv[1];
    uint32_t rlcore_ip = ipv4_to_host_order(argv[2]);
    uint16_t rlcore_port = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : kRlCoreDefaultPort;

    UdpTransport transport;
    transport.bind(0);

    std::atomic<int> received{0};
    transport.set_topic_handler(TOPIC_PUNCH_TEST, [&](const uint8_t* payload, size_t len) {
        std::string msg(reinterpret_cast<const char*>(payload), len);
        std::printf("[%s] RECEIVED: \"%s\"\n", role.c_str(), msg.c_str());
        ++received;
    });
    transport.start();

    // NOTE: self_ip here is only used as the SELF-REPORTED fallback --
    // when rlcore is run with --nat, it ignores this and uses the
    // observed UDP source address instead, which is the whole point.
    uint32_t topics[1] = {TOPIC_PUNCH_TEST};

    std::printf("[%s] registering (attempt 1/2) with rlcore at %s:%u...\n",
                role.c_str(), argv[2], rlcore_port);
    // transport.start() (above) already owns recvfrom() on this socket via
    // its dedicated data thread -- pass &transport so the RegisterAck is
    // handed off through transport->get_register_reply() instead of racing
    // that thread with a second recvfrom() here (see the contract on
    // register_with_rlcore_on_socket() itself). Without this, the data
    // thread wins the race for the ACK almost every time and this call
    // times out and retries even though rlcore already replied correctly.
    auto outcome1 = register_with_rlcore_on_socket(
        transport.native_handle(), rlcore_ip, rlcore_port,
        0 /* self_ip unused server-side in --nat mode */, transport.local_port(),
        topics, 1, /*max_retries=*/3, /*timeout_ms=*/500, &transport);
    std::printf("[%s] attempt 1: ok=%d peers=%zu\n", role.c_str(), outcome1.ok, outcome1.peers.size());

    std::printf("[%s] waiting 3s for the other side to register too...\n", role.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::printf("[%s] registering (attempt 2/2)...\n", role.c_str());
    auto outcome2 = register_with_rlcore_on_socket(
        transport.native_handle(), rlcore_ip, rlcore_port,
        0, transport.local_port(), topics, 1,
        /*max_retries=*/3, /*timeout_ms=*/500, &transport);
    std::printf("[%s] attempt 2: ok=%d peers=%zu\n", role.c_str(), outcome2.ok, outcome2.peers.size());

    std::vector<PeerAddr> peers;
    for (const auto& p : outcome1.peers) peers.push_back(PeerAddr{p.ip, p.port});
    for (const auto& p : outcome2.peers) peers.push_back(PeerAddr{p.ip, p.port});

    if (peers.empty()) {
        std::printf("[%s] FAIL: no peer learned from rlcore -- cannot test punching\n", role.c_str());
        transport.stop();
        return 1;
    }

    for (const auto& peer : peers) {
        struct in_addr ia{}; ia.s_addr = htonl(peer.ip_host_order);
        std::printf("[%s] learned peer: %s:%u -- punching...\n", role.c_str(), inet_ntoa(ia), peer.port);
        for (int i = 0; i < 5; ++i) {
            transport.publish_raw(kNatPunchTopicId, nullptr, 0, peer);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::printf("[%s] punch burst done, sending real test messages...\n", role.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 5; ++i) {
        std::string msg = role + "-msg-" + std::to_string(i);
        for (const auto& peer : peers) {
            transport.publish_raw(TOPIC_PUNCH_TEST, msg.data(), msg.size(), peer);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::printf("[%s] waiting up to 10s for inbound messages...\n", role.c_str());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && received.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (received.load() > 0) {
        std::printf("[%s] RESULT: PASS -- received %d message(s) from the other side "
                    "(NAT hole punching worked)\n", role.c_str(), received.load());
    } else {
        std::printf("[%s] RESULT: FAIL -- received 0 messages (hole punching did not "
                    "succeed for this direction)\n", role.c_str());
    }

    transport.stop();
    return received.load() > 0 ? 0 : 1;
}
