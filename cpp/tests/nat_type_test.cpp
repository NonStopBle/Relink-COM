// Real cross-role NAT test that exercises message-type DIVERSITY, not
// node count (see nat_stress_test.cpp for that axis): one fixed-size
// primitive, one composite std_msgs type, one geometry_msgs type, one
// sensor_msgs type, and -- the one that actually matters for NAT --
// Image, ReLink's only multi-packet type (advertise_image/publish_image/
// subscribe_image, chunked via image.hpp). Every other type here fits
// one UDP datagram and only proves registration/punching once; Image
// proves that ALL of a burst's chunks survive the same punched NAT
// mapping in order, which a single-packet type can't exercise.
//
// Both roles advertise AND subscribe every topic (symmetric), so this
// doubles as a two-way punch proof like nat_punch_test.cpp, just with
// real typed payloads instead of a raw string.
//
// usage: nat_type_test <a|b> <rlcore_ip> [rlcore_port]

#include "relink/relink.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

using namespace relink;
using namespace relink::std_msgs;
using namespace relink::geometry_msgs;
using namespace relink::sensor_msgs;

constexpr int kMessagesPerType = 10;
constexpr size_t kImageBytes = 50000; // ~50KB -> multiple ImageChunk fragments

struct Counts {
    std::atomic<int> bool_n{0};
    std::atomic<int> float_n{0};
    std::atomic<int> string_n{0};
    std::atomic<int> pose_n{0};
    std::atomic<int> navsat_n{0};
    std::atomic<int> image_n{0};
    std::atomic<int> image_bytes_ok{0};
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <a|b> <rlcore_ip> [rlcore_port]\n", argv[0]);
        return 2;
    }
    std::string role = argv[1];
    std::string rlcore_ip = argv[2];
    uint16_t rlcore_port = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 8445;

    RelinkNode node;
    node.set_rlcore.ip(rlcore_ip);
    node.set_rlcore.port(rlcore_port);
    node.set_multiplex(false); // each type gets its own dedicated port/NAT mapping

    Counts counts;

    node.advertise<Bool>("/nat_type/bool");
    node.subscribe<Bool>("/nat_type/bool", [&](const Bool& m) {
        if (m.data == 1) ++counts.bool_n;
    });

    node.advertise<Float64>("/nat_type/float64");
    node.subscribe<Float64>("/nat_type/float64", [&](const Float64&) {
        ++counts.float_n;
    });

    node.advertise<String>("/nat_type/string");
    node.subscribe<String>("/nat_type/string", [&](const String& m) {
        if (m.str().rfind("hello-", 0) == 0) ++counts.string_n;
    });

    node.advertise<Pose>("/nat_type/pose");
    node.subscribe<Pose>("/nat_type/pose", [&](const Pose&) {
        ++counts.pose_n;
    });

    node.advertise<NavSatFix>("/nat_type/navsat");
    node.subscribe<NavSatFix>("/nat_type/navsat", [&](const NavSatFix&) {
        ++counts.navsat_n;
    });

    node.advertise_image("/nat_type/image");
    node.subscribe_image("/nat_type/image", [&](uint32_t, const std::vector<uint8_t>& bytes) {
        ++counts.image_n;
        if (bytes.size() == kImageBytes && bytes.front() == 0xAB && bytes.back() == 0xCD) {
            ++counts.image_bytes_ok;
        }
    });

    std::printf("[%s] spinning up, registering with rlcore at %s:%u...\n",
                role.c_str(), rlcore_ip.c_str(), rlcore_port);

    // Give both sides time to register + punch before sending real
    // traffic -- spin_once() drives periodic re-registration, which is
    // also what teaches this node about the other side's peer address.
    for (int i = 0; i < 40; ++i) {
        node.spin_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::vector<uint8_t> image_payload(kImageBytes);
    image_payload.front() = 0xAB;
    image_payload.back() = 0xCD;
    for (size_t i = 1; i + 1 < image_payload.size(); ++i) {
        image_payload[i] = static_cast<uint8_t>(i);
    }

    std::printf("[%s] sending %d messages per type...\n", role.c_str(), kMessagesPerType);
    for (int i = 0; i < kMessagesPerType; ++i) {
        node.spin_once();

        Bool b{1};
        node.publish<Bool>("/nat_type/bool", b);

        Float64 f{3.14159 * i};
        node.publish<Float64>("/nat_type/float64", f);

        String s{};
        s.set(("hello-" + role + "-" + std::to_string(i)).c_str());
        node.publish<String>("/nat_type/string", s);

        Pose p{};
        p.position = Point{1.0f * i, 2.0f * i, 3.0f * i};
        p.orientation = Quaternion{0, 0, 0, 1};
        node.publish<Pose>("/nat_type/pose", p);

        NavSatFix nf{};
        nf.header.stamp_now();
        nf.header.set_frame_id("gps");
        nf.status = NavSatStatus{NavSatStatus::kFix, NavSatStatus::kServiceGps};
        nf.latitude = 37.0 + i * 0.001;
        nf.longitude = -122.0 - i * 0.001;
        nf.altitude = 10.0;
        node.publish<NavSatFix>("/nat_type/navsat", nf);

        node.publish_image("/nat_type/image", image_payload.data(), image_payload.size());

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    std::printf("[%s] done sending, waiting up to 5s for stragglers...\n", role.c_str());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        node.spin_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto pass = [](int got, int want) { return got >= want ? "OK" : "FAIL"; };
    std::printf("\n[%s] === RESULT ===\n", role.c_str());
    std::printf("[%s] Bool:     %d/%d %s\n", role.c_str(), counts.bool_n.load(), kMessagesPerType, pass(counts.bool_n.load(), kMessagesPerType));
    std::printf("[%s] Float64:  %d/%d %s\n", role.c_str(), counts.float_n.load(), kMessagesPerType, pass(counts.float_n.load(), kMessagesPerType));
    std::printf("[%s] String:   %d/%d %s\n", role.c_str(), counts.string_n.load(), kMessagesPerType, pass(counts.string_n.load(), kMessagesPerType));
    std::printf("[%s] Pose:     %d/%d %s\n", role.c_str(), counts.pose_n.load(), kMessagesPerType, pass(counts.pose_n.load(), kMessagesPerType));
    std::printf("[%s] NavSatFix:%d/%d %s\n", role.c_str(), counts.navsat_n.load(), kMessagesPerType, pass(counts.navsat_n.load(), kMessagesPerType));
    std::printf("[%s] Image:    %d/%d complete, %d/%d byte-exact %s\n", role.c_str(),
                counts.image_n.load(), kMessagesPerType, counts.image_bytes_ok.load(), kMessagesPerType,
                pass(counts.image_bytes_ok.load(), kMessagesPerType));

    bool all_ok = counts.bool_n.load() >= kMessagesPerType &&
                  counts.float_n.load() >= kMessagesPerType &&
                  counts.string_n.load() >= kMessagesPerType &&
                  counts.pose_n.load() >= kMessagesPerType &&
                  counts.navsat_n.load() >= kMessagesPerType &&
                  counts.image_bytes_ok.load() >= kMessagesPerType;

    return all_ok ? 0 : 1;
}
