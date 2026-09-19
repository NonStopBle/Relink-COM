// diagnostic_msgs_pubsub -- publishes and subscribes every
// diagnostic_msgs type ReLink ships (relink/include/relink/
// standard_msgs.hpp), one topic per type: KeyValue, DiagnosticStatus,
// DiagnosticArray.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/diagnostic_msgs_pubsub.cpp -o diagnostic_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./diagnostic_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace diagnostic_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<KeyValue>("/relink/diag/key_value", [](const KeyValue& m) {
        std::printf("diagnostic_msgs/KeyValue %s=%s\n", m.key_str().c_str(), m.value_str().c_str());
    });
    node.subscribe<DiagnosticStatus>("/relink/diag/status", [](const DiagnosticStatus& m) {
        std::printf("diagnostic_msgs/DiagnosticStatus level=%d name=%s\n", m.level, m.name);
    });
    node.subscribe<DiagnosticArray>("/relink/diag/array", [](const DiagnosticArray& m) {
        std::printf("diagnostic_msgs/DiagnosticArray status_count=%u\n", m.status_count);
    });

    node.advertise<KeyValue>("/relink/diag/key_value");
    node.advertise<DiagnosticStatus>("/relink/diag/status");
    node.advertise<DiagnosticArray>("/relink/diag/array");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        {
            KeyValue kv{};
            kv.set_key("battery_pct");
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", 100 - (i % 100));
            kv.set_value(buf);
            node.publish<KeyValue>("/relink/diag/key_value", kv);
        }
        {
            DiagnosticStatus status{};
            status.level = DiagnosticStatus::kOk;
            status.set_name("battery_monitor");
            status.set_message("nominal");
            status.set_hardware_id("bms_01");
            node.publish<DiagnosticStatus>("/relink/diag/status", status);
        }
        {
            DiagnosticArray arr{};
            arr.header.set_frame_id("diagnostics");
            arr.status_count = 1;
            arr.status[0].level = DiagnosticStatus::kOk;
            arr.status[0].set_name("battery_monitor");
            node.publish<DiagnosticArray>("/relink/diag/array", arr);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}
