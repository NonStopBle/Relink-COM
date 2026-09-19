// custom_types_pubsub -- one node publishing and subscribing several
// different message types at once: a built-in type (Bool), and two
// user-defined custom types (Pose2D, a small struct; and a fixed-size
// array-of-floats type). Shows that "any message type" really does
// just mean "any trivially-copyable, fixed-layout type" -- there's no
// registration step, no codegen, no schema exchange beyond both sides
// independently defining the identical struct layout (Step 7 of the
// README).
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/custom_types_pubsub.cpp -o custom_types_pubsub
// Run (in two terminals, or on two machines):
//   ./custom_types_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>
#include <cmath>

// --- user-defined message types -- these are the caller's own code,
// not part of the ReLink library. Any trivially-copyable struct works;
// #pragma pack(1) keeps the layout identical to Python's ctypes
// equivalent with _pack_ = 1 (Step 7). ---

#pragma pack(push, 1)
struct Pose2D {
    float x, y, theta;
    uint32_t seq;
};

struct Waypoints {
    float x[4];
    float y[4];
    uint8_t count;
};
#pragma pack(pop)

const char* TOPIC_ARMED = "/relink/armed";      // built-in Bool
const char* TOPIC_POSE = "/relink/pose";        // custom Pose2D
const char* TOPIC_PATH = "/relink/path";        // custom Waypoints

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    // Subscribe to all three before advertising any of them, so an
    // early message from a peer that started first is never missed.
    node.subscribe<Bool>(TOPIC_ARMED, [](const Bool& msg) {
        std::printf("armed:  %s\n", msg.data ? "true" : "false");
    });
    node.subscribe<Pose2D>(TOPIC_POSE, [](const Pose2D& msg) {
        std::printf("pose:   seq=%u x=%.2f y=%.2f theta=%.2f\n",
                    msg.seq, msg.x, msg.y, msg.theta);
    });
    node.subscribe<Waypoints>(TOPIC_PATH, [](const Waypoints& msg) {
        std::printf("path:   %u point(s), first=(%.2f, %.2f)\n",
                    msg.count, msg.x[0], msg.y[0]);
    });

    node.advertise<Bool>(TOPIC_ARMED);
    node.advertise<Pose2D>(TOPIC_POSE);
    node.advertise<Waypoints>(TOPIC_PATH);

    uint32_t seq = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        // Bool: toggles once every 10 iterations, just to show it moving.
        node.publish<Bool>(TOPIC_ARMED, Bool{ .data = (seq / 10) % 2 == 0 });

        // Pose2D: a small circle, so the numbers visibly change every tick.
        float t = static_cast<float>(seq) * 0.1f;
        Pose2D pose{ .x = std::cos(t), .y = std::sin(t), .theta = t, .seq = seq };
        node.publish<Pose2D>(TOPIC_POSE, pose);

        // Waypoints: a fixed-size array message -- the whole struct,
        // arrays included, is one flat memcpy on the wire, same as any
        // other trivially-copyable type.
        Waypoints path{};
        path.count = 4;
        for (int i = 0; i < 4; ++i) {
            path.x[i] = static_cast<float>(i);
            path.y[i] = static_cast<float>(i * i);
        }
        node.publish<Waypoints>(TOPIC_PATH, path);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++seq;
    }
}
