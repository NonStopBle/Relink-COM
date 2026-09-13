// ReLink-com usage example -- publisher + subscriber, mode B (multicast,
// no daemon). Same shape as relink_example.cpp (at the repo root, mode A)
// but with node.use_multicast_discovery() instead of set_rlcore, to
// show both discovery modes have the same downstream API.
//
// Two processes: run with argv[1] == "pub" or "sub".

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

enum Topics : uint16_t { TOPIC_COUNTER = 300 };

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub]\n", argv[0]);
        return 1;
    }
    const bool is_publisher = (std::strcmp(argv[1], "pub") == 0);

    RelinkNode node;
    node.use_multicast_discovery();

    if (is_publisher) {
        node.advertise<Int32>(TOPIC_COUNTER);
        std::printf("publisher: advertising TOPIC_COUNTER via multicast discovery\n");

        int i = 0;
        while (true) {
            node.publish<Int32>(TOPIC_COUNTER, Int32{ .data = i });
            node.spin_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++i;
        }
    } else {
        node.subscribe<Int32>(TOPIC_COUNTER, [](const Int32& msg) {
            std::printf("counter: %d\n", msg.data);
        });
        std::printf("subscriber: waiting for messages on TOPIC_COUNTER via multicast discovery\n");
        node.spin();
    }
    return 0;
}
