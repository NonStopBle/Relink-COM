// pubsub -- talker and listener combined into a single node/file.
//
// talker.cpp and listener.cpp show the two roles split apart, the way
// the classic ROS tutorial does it. In practice a ReLink node is very
// often both at once (there's no separate "node type" for publishers
// vs. subscribers) -- this file shows that: run the same binary on two
// (or more) machines, or two terminals on one machine, and every copy
// both broadcasts its own counted "hello world" message and prints
// whatever the other copies send.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/pubsub.cpp -o pubsub
// Run (in two terminals, or on two machines):
//   ./pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

// Same fixed-size message shape as talker.cpp/listener.cpp -- ReLink's
// advertise<T>/subscribe<T>/publish<T> only accept trivially-copyable
// types, so a fixed buffer stands in for std_msgs/String's dynamic one.
struct Chatter {
    char data[128];
};

// Same topic name used by talker.cpp/listener.cpp: run this against
// either of them (or another copy of itself) and they'll all hear each
// other, since the name hashes to the same wire id everywhere.
const char* TOPIC_CHATTER = "/relink/chatter";

int main() {
    RelinkNode node;

    // Multicast discovery: zero setup, no master/roscore, no daemon to
    // start first. See rlcore_pubsub.cpp for the alternative.
    node.use_multicast_discovery();

    // Subscribe first so we don't miss any early messages, then
    // advertise -- both calls just declare intent, nothing is sent yet.
    // This is the listener half: the callback fires inline on ReLink's
    // background data thread whenever a message from *any* peer
    // (including, on the same machine, our own loopback copy of the
    // multicast beacon -- but not our own publish() traffic, ReLink
    // never delivers your own published messages back to you) arrives.
    node.subscribe<Chatter>(TOPIC_CHATTER, [](const Chatter& msg) {
        std::printf("I heard: [%s]\n", msg.data);
    });
    node.advertise<Chatter>(TOPIC_CHATTER);

    // Resolve the topic name to its wire id once (idempotent), so we
    // can report how many peers we've found so far -- purely informational.
    uint32_t topic_id = node.topic_id_for(TOPIC_CHATTER);

    // No ros::Rate/loop_rate helper in ReLink -- pace the loop with a
    // plain sleep_for. 10 Hz, same rate as the ROS tutorial.
    const auto period = std::chrono::milliseconds(100);

    int count = 0;
    while (true) {
        // spin_once() services discovery; incoming messages are
        // delivered on their own thread, no polling needed for them --
        // this is purely the publisher half's own housekeeping.
        node.spin_once();

        Chatter msg{};
        std::ostringstream ss;
        ss << "hello world " << count;
        std::string text = ss.str();
        std::strncpy(msg.data, text.c_str(), sizeof(msg.data) - 1);

        std::printf("sent:     %s\n", msg.data);
        node.publish<Chatter>(TOPIC_CHATTER, msg);

        if (node.peers_for_topic(topic_id).empty()) {
            std::printf("(no peers found yet -- is another copy of this program running on the network?)\n");
        }

        std::this_thread::sleep_for(period);
        ++count;
    }
}
