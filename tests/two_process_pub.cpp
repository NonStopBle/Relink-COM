// Step 7: two-process correctness test -- publisher side.
// usage: two_process_pub <comcore|multicast> [comcore_ip] <topic_id> <count>
//
// Publishes `count` sequential Int32 messages (data = 0..count-1) on
// `topic_id`, with zero manually-configured peer address -- discovery
// (mode A or B) resolves the subscriber automatically, per spec.

#include "relink/relink.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <comcore|multicast> [comcore_ip] <topic_id> <count>\n", argv[0]);
        return 2;
    }

    RelinkNode node;
    int argi = 1;
    std::string mode = argv[argi++];
    if (mode == "comcore") {
        std::string ip = argv[argi++];
        node.set_com_core.ip(ip);
    } else if (mode == "multicast") {
        node.use_multicast_discovery();
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }

    uint16_t topic_id = static_cast<uint16_t>(std::atoi(argv[argi++]));
    int count = std::atoi(argv[argi++]);

    node.advertise<Int32>(topic_id);

    // spin_once() triggers discovery/registration once; give it a moment
    // to actually resolve peers before blasting messages (mirrors real
    // usage where a node registers, then starts publishing).
    node.spin_once();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto peers = node.peers_for_topic(topic_id);
    std::printf("pub: resolved %zu peer(s) for topic %u\n", peers.size(), topic_id);

    for (int i = 0; i < count; ++i) {
        node.publish<Int32>(topic_id, Int32{i});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::printf("pub: done, sent %d messages\n", count);
    // Give the last few datagrams time to land before the process exits.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 0;
}
