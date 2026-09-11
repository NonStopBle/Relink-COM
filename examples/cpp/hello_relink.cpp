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

// Every ReLink topic needs a wire id, but you don't have to hand-assign
// a number: give advertise/subscribe/publish a human-readable name
// instead and ReLink hashes it down to the uint32_t that actually goes
// on the wire (see RelinkNode::topic_id_for in relink.hpp). Both sides
// just need to type the same string -- no shared constant/header needed.
// A numeric id (e.g. `constexpr uint32_t TOPIC_HELLO = 42;`) still works
// exactly as before if you'd rather assign ids by hand.
const char* TOPIC_HELLO = "/relink/hello";

int main() {
    RelinkNode node;

    // Multicast discovery: zero setup, no daemon to run first. See
    // rlcore_pubsub.cpp for the alternative (a small daemon, useful
    // when multicast isn't available on your network).
    node.use_multicast_discovery();

    // Subscribe first so we don't miss any early messages, then
    // advertise -- both calls just declare intent, nothing is sent yet.
    node.subscribe<Int32>(TOPIC_HELLO, [](const Int32& msg) {
        std::printf("received: %d\n", msg.data);
    });
    node.advertise<Int32>(TOPIC_HELLO);

    // peers_for_topic() still takes the numeric wire id -- resolve the
    // name once via topic_id_for() (it's idempotent: calling it again
    // with the same name just returns the same id from the registry).
    uint32_t topic_id = node.topic_id_for(TOPIC_HELLO);

    int counter = 0;
    while (true) {
        node.spin_once();  // services discovery; incoming messages are
                            // delivered on their own thread, no polling
                            // needed for receiving

        size_t peer_count = node.peers_for_topic(topic_id).size();
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
