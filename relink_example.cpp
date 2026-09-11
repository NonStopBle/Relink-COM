// ReLink-com usage example — publisher + subscriber, mode A (rlcore)
//
// This is USAGE code showing how an application would call the ReLink API
// once it's built per relink-com-spec.md. It is not the library
// implementation itself (that's what the spec's task list builds).
//
// Two processes: run with argv[1] == "pub" or "sub" to pick a role.

#include "relink/relink.hpp"   // the ReLink library (per spec)
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

// --- user's own message definitions (their own header, per spec's ---
// --- "Custom types" section — not part of the ReLink library itself) --

#pragma pack(push, 1)
struct ImuReading {
    float    accel_x, accel_y, accel_z;
    float    gyro_x,  gyro_y,  gyro_z;
    uint64_t timestamp_us;
};
#pragma pack(pop)

// user picks their own topic IDs
enum MyTopics : uint16_t {
    TOPIC_IMU  = 100,
    TOPIC_TEMP = 101,
};

// --- helper: current time in microseconds, for the timestamp field ---
static uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub]\n", argv[0]);
        return 1;
    }
    const bool is_publisher = (std::strcmp(argv[1], "pub") == 0);

    RelinkNode node;

    // --- discovery config: mode A (rlcore), per spec ---
    // Both processes point at the same rlcore instance. Port defaults
    // to 8445, so it's not set explicitly here unless rlcore was
    // started on a different port.
    node.set_rlcore.ip("10.0.0.5");
    // node.set_rlcore.port(9000);  // only if rlcore uses a non-default port

    // NOTE: calling node.use_multicast_discovery() here too would be a
    // configuration error (mutually exclusive with rlcore) — see spec.

    if (is_publisher) {
        // --- publisher side ---
        // secure=false, checksum=false (both default) — plaintext,
        // zero overhead, matches the 1000Hz hot-path target.
        node.advertise<ImuReading>(TOPIC_IMU);

        // Also advertise a default std_msgs-style type on another topic,
        // to show both default and custom types working side by side.
        node.advertise<Float32>(TOPIC_TEMP);

        std::printf("publisher: advertising TOPIC_IMU and TOPIC_TEMP\n");

        // Publish at roughly 1000Hz (1ms sleep). In a real embedded/
        // real-time build this loop would live on ReLink's own pinned
        // data thread per the spec's Threading section — this example
        // just drives it from main() for simplicity.
        int i = 0;
        while (true) {
            ImuReading reading{
                .accel_x = 0.01f * i, .accel_y = 0.02f, .accel_z = 9.81f,
                .gyro_x  = 0.0f,      .gyro_y  = 0.0f,  .gyro_z  = 0.0f,
                .timestamp_us = now_us()
            };
            node.publish<ImuReading>(TOPIC_IMU, reading);

            if (i % 100 == 0) {
                // publish a temperature reading every ~100ms
                node.publish<Float32>(TOPIC_TEMP, Float32{ .data = 36.6f });
            }

            node.spin_once();  // service the data/discovery loop once
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++i;
        }

    } else {
        // --- subscriber side ---
        // Callback runs on ReLink's data thread (per spec) — must stay
        // fast and non-blocking. Here we just print, which is fine for
        // an example but would be too slow for a real 1000Hz callback
        // in production code (printf is not cheap) — hand off to a
        // worker thread for anything heavier than this.
        node.subscribe<ImuReading>(TOPIC_IMU, [](const ImuReading& msg) {
            std::printf("imu: accel=(%.3f, %.3f, %.3f) t=%llu\n",
                        msg.accel_x, msg.accel_y, msg.accel_z,
                        (unsigned long long)msg.timestamp_us);
        });

        node.subscribe<Float32>(TOPIC_TEMP, [](const Float32& msg) {
            std::printf("temp: %.1f C\n", msg.data);
        });

        std::printf("subscriber: waiting for messages on TOPIC_IMU and TOPIC_TEMP\n");

        node.spin();  // blocks, runs the data + discovery loop forever
    }

    return 0;
}
