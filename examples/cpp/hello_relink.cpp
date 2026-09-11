// hello_relink -- the simplest possible ReLink program.
//
// Run this same program on two (or more) machines on the same network,
// or in two terminal windows on one machine, and they will find each
// other automatically (no setup, no config file, no separate daemon)
// and start exchanging messages -- each copy is both a publisher and a
// subscriber, sending a counter once a second and printing whatever it
// receives from the others.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/hello_relink.cpp -o hello_relink
// Run (in two terminals, or on two machines):
//   ./hello_relink

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

// Every ReLink topic needs a numeric ID. Pick any number -- both sides
// just need to agree on it, the same way both ends of the program agree
// here by using the same constant.
constexpr uint16_t TOPIC_HELLO = 42;

int main() {
    RelinkNode node;

    // Multicast discovery: zero setup, no daemon to run first. See
    // comcore_pubsub.cpp for the alternative (a small daemon, useful
    // when multicast isn't available on your network).
    node.use_multicast_discovery();

    // Subscribe first so we don't miss any early messages, then
    // advertise -- both calls just declare intent, nothing is sent yet.
    node.subscribe<Int32>(TOPIC_HELLO, [](const Int32& msg) {
        std::printf("received: %d\n", msg.data);
    });
    node.advertise<Int32>(TOPIC_HELLO);

    int counter = 0;
    while (true) {
        node.spin_once();  // services discovery; incoming messages are
                            // delivered on their own thread, no polling
                            // needed for receiving

        size_t peer_count = node.peers_for_topic(TOPIC_HELLO).size();
        if (peer_count == 0) {
            // No other copy of this program has been found yet. This is
            // normal for the first second or two -- multicast discovery
            // finds peers via a short burst of "here I am" broadcasts on
            // startup, so give it a moment.
            std::printf("(no peers found yet -- is another copy of this "
                        "program running on the network?)\n");
        } else {
            node.publish<Int32>(TOPIC_HELLO, Int32{ .data = counter });
            std::printf("sent:     %d (to %zu peer(s))\n", counter, peer_count);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++counter;
    }
}
