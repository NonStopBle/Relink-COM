// Step 7: two-process correctness test -- subscriber side.
// usage: two_process_sub <rlcore|multicast> [rlcore_ip] <topic_id> <expected_count> <timeout_sec>
//
// Subscribes on `topic_id` with zero manually-configured peer address,
// waits up to timeout_sec for `expected_count` distinct Int32 values
// (0..expected_count-1) to arrive, then prints a machine-checkable
// RESULT line and exits 0 (PASS) or 1 (FAIL).

#include "relink/relink.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <set>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <rlcore|multicast> [rlcore_ip] <topic_id> <expected_count> <timeout_sec>\n", argv[0]);
        return 2;
    }

    RelinkNode node;
    int argi = 1;
    std::string mode = argv[argi++];
    if (mode == "rlcore") {
        std::string ip = argv[argi++];
        node.set_rlcore.ip(ip);
    } else if (mode == "multicast") {
        node.use_multicast_discovery();
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 2;
    }

    uint32_t topic_id = static_cast<uint32_t>(std::atoi(argv[argi++]));
    int expected_count = std::atoi(argv[argi++]);
    int timeout_sec = std::atoi(argv[argi++]);

    std::mutex mtx;
    std::set<int> received;

    node.subscribe<Int32>(topic_id, [&](const Int32& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        received.insert(msg.data);
    });

    node.spin_once(); // triggers discovery registration

    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (static_cast<int>(received.size()) >= expected_count) break;
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(timeout_sec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::lock_guard<std::mutex> lock(mtx);
    bool all_present = (static_cast<int>(received.size()) == expected_count);
    if (all_present) {
        for (int i = 0; i < expected_count; ++i) {
            if (!received.count(i)) { all_present = false; break; }
        }
    }

    std::printf("sub: received %zu/%d unique messages\n", received.size(), expected_count);
    if (all_present) {
        std::printf("RESULT: PASS\n");
        return 0;
    } else {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
}
