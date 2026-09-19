// main -- conventional src/main.cpp entry point, functionally identical
// to pubsub.cpp (publisher + subscriber combined in one node). If your
// build tooling expects a src/main.cpp, use this one; pub.cpp/sub.cpp/
// pubsub.cpp exist alongside it for the split-role variants -- see
// README.md in this directory.

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

struct Chatter {
    char data[128];
};

const char* TOPIC_CHATTER = "/example/chatter";

int main() {
    RelinkNode node;
    node.use_multicast_discovery();

    // Subscribe first so we don't miss any early messages, then
    // advertise -- both calls just declare intent, nothing is sent yet.
    node.subscribe<Chatter>(TOPIC_CHATTER, [](const Chatter& msg) {
        std::printf("received: %s\n", msg.data);
    });
    node.advertise<Chatter>(TOPIC_CHATTER);

    uint32_t topic_id = node.topic_id_for(TOPIC_CHATTER);
    const auto period = std::chrono::milliseconds(500);

    int count = 0;
    while (true) {
        node.spin_once();

        Chatter msg{};
        std::ostringstream ss;
        ss << "hello " << count;
        std::string text = ss.str();
        std::strncpy(msg.data, text.c_str(), sizeof(msg.data) - 1);

        std::printf("sent:     %s\n", msg.data);
        node.publish<Chatter>(TOPIC_CHATTER, msg);

        if (node.peers_for_topic(topic_id).empty()) {
            std::printf("(no peers found yet -- run another copy of this program on the same network)\n");
        }

        std::this_thread::sleep_for(period);
        ++count;
    }
}
