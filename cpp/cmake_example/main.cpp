// Minimal pub/sub example for the cpp/cmake_example/ template project.
// Every copy of this binary both publishes and subscribes on the same
// topic -- run it in two terminals (or on two machines on the same
// LAN) and each copy will print what the other one sends.

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

    // Multicast discovery: zero setup, no daemon to start first. Use
    // node.set_rlcore.ip("...") instead if you're using Mode A
    // (rlcore) -- see the README's "Discovery mode" section.
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
        node.spin_once(); // services discovery; incoming messages arrive on their own thread

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
