// actionlib_msgs_pubsub -- publishes and subscribes every
// actionlib_msgs type ReLink ships (relink/include/relink/
// standard_msgs.hpp), one topic per type: GoalID, GoalStatus,
// GoalStatusArray.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/actionlib_msgs_pubsub.cpp -o actionlib_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./actionlib_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace actionlib_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<GoalID>("/relink/action/goal_id", [](const GoalID& m) {
        std::printf("actionlib_msgs/GoalID id=%s\n", m.id_str().c_str());
    });
    node.subscribe<GoalStatus>("/relink/action/goal_status", [](const GoalStatus& m) {
        std::printf("actionlib_msgs/GoalStatus status=%u text=%s\n", m.status, m.text_str().c_str());
    });
    node.subscribe<GoalStatusArray>("/relink/action/goal_status_array", [](const GoalStatusArray& m) {
        std::printf("actionlib_msgs/GoalStatusArray status_list_count=%u\n", m.status_list_count);
    });

    node.advertise<GoalID>("/relink/action/goal_id");
    node.advertise<GoalStatus>("/relink/action/goal_status");
    node.advertise<GoalStatusArray>("/relink/action/goal_status_array");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        {
            GoalID gid{};
            gid.stamp = std_msgs::Time::now();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "goal_%d", i);
            gid.set_id(buf);
            node.publish<GoalID>("/relink/action/goal_id", gid);
        }
        {
            GoalStatus gs{};
            gs.status = GoalStatus::kActive;
            gs.set_text("moving to goal");
            node.publish<GoalStatus>("/relink/action/goal_status", gs);
        }
        {
            GoalStatusArray gsa{};
            gsa.header.set_frame_id("action_server");
            gsa.status_list_count = 1;
            gsa.status_list[0].status = GoalStatus::kActive;
            node.publish<GoalStatusArray>("/relink/action/goal_status_array", gsa);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}
