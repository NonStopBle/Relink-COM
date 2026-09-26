// sub -- subscriber-only half of a pub/sub pair. Run alongside pub.cpp
// (or pubsub.cpp on either end) -- see README.md in this directory.

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

struct Chatter {
    char data[128];
};

const char* TOPIC_CHATTER = "/example/chatter";

int main() {
    RelinkNode node;
    // node.use_multicast_discovery();


    node.set_rlcore.ip("43.228.86.96");
    node.set_rlcore.port(8445);
    node.set_rlcore.setRelay(true);
    node.set_multiplex(false);

    // Subscribe before calling spin_once()/entering the loop, so we
    // don't miss any messages sent right after discovery completes.
    // The callback fires inline on ReLink's background data thread --
    // keep it fast, and don't block in it.
    node.subscribe<Chatter>(TOPIC_CHATTER, [](const Chatter& msg) {
        std::printf("received: %s\n", msg.data);
    });

    // There's nothing else to do in the main thread -- messages arrive
    // on their own thread, not via polling. spin_once() in a loop is
    // still needed to service discovery on the publisher's side of a
    // pub/sub pair, but a pure subscriber can just block here.
    while (true) {
        node.spin_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
