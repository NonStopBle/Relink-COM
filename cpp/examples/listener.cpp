// listener -- the simplest possible ReLink subscriber node.
//
// This is ReLink's answer to the classic ROS "listener" tutorial:
// it subscribes to the chatter topic and prints whatever talker.cpp
// broadcasts. No master/roscore, no explicit connection setup -- just
// run this and talker on the same network (or same machine) and
// multicast discovery does the rest.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/listener.cpp -o listener
// Run (in another terminal, or on another machine on the same network):
//   ./listener
//   ./talker

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

// Must match talker.cpp's Chatter definition byte-for-byte -- ReLink
// does no schema negotiation between nodes, the same way ROS relies on
// both sides being built against the same generated message header.
struct Chatter {
    char data[128];
};

// Same topic name as talker.cpp -- both sides hash it to the same
// wire id independently, no shared header/constant needed.
const char* TOPIC_CHATTER = "/relink/chatter";

// This is the callback that fires when a new message has arrived on
// "/relink/chatter". Unlike ROS's chatterCallback(const
// std_msgs::String::ConstPtr&), ReLink hands you the struct by value
// (it's a small trivially-copyable POD, not a heap-allocated shared_ptr
// payload) -- there's nothing to keep alive past the callback.
void chatter_callback(const Chatter& msg) {
    std::printf("I heard: [%s]\n", msg.data);
}

int main() {
    RelinkNode node;

    // Multicast discovery: zero setup, no master/roscore to run first.
    node.use_multicast_discovery();

    // Subscribe to "/relink/chatter". ReLink invokes chatter_callback()
    // inline on its own background data thread as soon as a matching
    // message arrives -- there's no separate queue-size argument like
    // ROS's subscribe(topic, queue_size, cb); a message is delivered as
    // soon as it's received, not batched or throttled.
    node.subscribe<Chatter>(TOPIC_CHATTER, chatter_callback);

    // ReLink has no ros::spin() blocking-forever helper built in --
    // spin_once() in a loop (like hello_relink.cpp) or the library's own
    // node.spin() (which just loops spin_once() + sleep internally)
    // both work; here we use spin() directly since, unlike talker, this
    // node has nothing else to do between messages.
    node.spin();

    return 0;
}
