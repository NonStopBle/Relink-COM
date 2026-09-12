// talker -- the simplest possible ReLink publisher node.
//
// This is ReLink's answer to the classic ROS "talker" tutorial: it
// advertises a chatter topic and broadcasts a counted "hello world"
// string once a second, ten times a second... except ReLink has no
// master/roscore to register with -- multicast discovery (or rlcore,
// see rlcore_pubsub.cpp) finds listener nodes automatically.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/talker.cpp -o talker
// Run (in another terminal, or on another machine on the same network):
//   ./listener
//   ./talker

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

// ReLink's advertise<T>/publish<T> only accept trivially-copyable types
// (see relink.hpp) -- there is no built-in variable-length string type,
// because a length-prefixed field would defeat the fixed-size, no-heap
// wire format the rest of the library relies on. A fixed-size buffer is
// the direct equivalent of std_msgs/String for a byte-exact wire type.
struct Chatter {
    char data[128];
};

// Every ReLink topic needs a wire id, but you type a human-readable name
// instead of hand-assigning a number -- ReLink hashes it (FNV-1a) down
// to the uint32_t that actually goes on the wire. Both talker and
// listener just need to type the same string.
const char* TOPIC_CHATTER = "/relink/chatter";

int main() {
    RelinkNode node;

    // Multicast discovery: zero setup, no master/roscore to start first.
    // See rlcore_pubsub.cpp for the alternative (a small daemon), useful
    // when multicast isn't available on your network.
    node.use_multicast_discovery();

    // Tell ReLink we're going to publish Chatter messages on
    // "/relink/chatter". There is no queue-size argument like ROS's
    // advertise(topic, queue_size): ReLink sends immediately to every
    // currently-known peer on publish() and keeps no internal backlog,
    // so there's nothing to bound.
    node.advertise<Chatter>(TOPIC_CHATTER);

    // Resolve the topic name to its wire id once (idempotent -- calling
    // it again with the same string just returns the same id from the
    // per-node name registry), so we can check how many peers we have.
    uint32_t topic_id = node.topic_id_for(TOPIC_CHATTER);

    // ReLink has no ros::Rate/loop_rate helper -- pace the loop with a
    // plain sleep_for. 10 Hz, same rate as the ROS tutorial.
    const auto period = std::chrono::milliseconds(100);

    int count = 0;
    while (true) {
        // spin_once() services discovery (starts the background data
        // thread on first call, processes newly-learned peers); there
        // is no ros::ok()/Ctrl-C shutdown flag to check here -- kill the
        // process to stop it, same as hello_relink.cpp.
        node.spin_once();

        // Build the message. Just like std_msgs::String, it's a single
        // "data" field -- here a fixed 128-byte buffer instead of a
        // dynamically-sized std::string.
        Chatter msg{};
        std::ostringstream ss;
        ss << "hello world " << count;
        std::string text = ss.str();
        std::strncpy(msg.data, text.c_str(), sizeof(msg.data) - 1);

        std::printf("%s\n", msg.data);

        // publish() sends to every peer ReLink has discovered so far for
        // this topic; if none have been found yet it's a harmless no-op
        // (returns false), same as ROS silently dropping messages
        // published before any subscriber has connected.
        node.publish<Chatter>(TOPIC_CHATTER, msg);

        if (node.peers_for_topic(topic_id).empty()) {
            std::printf("(no peers found yet -- is ./listener running on the network?)\n");
        }

        std::this_thread::sleep_for(period);
        ++count;
    }
}
