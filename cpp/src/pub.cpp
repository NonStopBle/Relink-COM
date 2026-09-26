// pub -- publisher-only half of a pub/sub pair. Run alongside sub.cpp
// (or pubsub.cpp on either end) -- see README.md in this directory.

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

// A fixed-size buffer stands in for a dynamic string: ReLink's
// advertise<T>/publish<T> only accept trivially-copyable types (no
// heap allocation on the wire path).
struct Chatter {
    char data[128];
};

// Both pub and sub just need to type the same topic name -- ReLink
// hashes it (FNV-1a) down to the uint32_t that actually goes on the wire.
const char* TOPIC_CHATTER = "/example/chatter";

int main() {
    RelinkNode node;
    

    node.set_rlcore.ip("43.228.86.96");
    node.set_rlcore.port(8445);
    node.set_rlcore.setRelay(true);
    node.set_multiplex(false);
    

    // Multicast discovery: zero setup, works as long as pub/sub are on
    // the same LAN segment. For nodes on different networks, use
    // node.set_rlcore.ip("...") instead -- see README.md.
    // node.use_multicast_discovery();

    // node.advertise<Chatter>(TOPIC_CHATTER);
    uint32_t topic_id = node.topic_id_for(TOPIC_CHATTER);

    const auto period = std::chrono::milliseconds(500);
    int count = 0;
    while (true) {
        node.spin_once(); // services discovery -- call this every loop iteration

        Chatter msg{};
        std::ostringstream ss;
        ss << "hello " << count;
        std::string text = ss.str();
        std::strncpy(msg.data, text.c_str(), sizeof(msg.data) - 1);

        std::printf("sent: %s\n", msg.data);
        node.publish<Chatter>(TOPIC_CHATTER, msg);

        if (node.peers_for_topic(topic_id).empty()) {
            std::printf("(no peers found yet -- is sub running on the network?)\n");
        }

        std::this_thread::sleep_for(period);
        ++count;
    }
}
