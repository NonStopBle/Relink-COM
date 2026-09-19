// std_msgs_pubsub -- publishes and subscribes every std_msgs composite
// type ReLink ships (relink/include/relink/standard_msgs.hpp), one
// topic per type: Empty, Time, Duration, ColorRGBA, Header, String.
// (The std_msgs primitives -- Bool, Int32, Float32, etc. -- are covered
// separately by builtin_types_pubsub.cpp, since they live in wire.hpp
// rather than standard_msgs.hpp.)
//
// Also demonstrates std_msgs's *MultiArray family (ROS's ByteMultiArray,
// Int8/16/32/64MultiArray, UInt8/16/32/64MultiArray, Float32/64MultiArray
// -- 11 types). standard_msgs.hpp does NOT ship these as ready-made
// structs: it only documents the underlying length-prefixed layout
// (wire.hpp's MultiArrayHeader) and says to build the concrete type
// yourself. That is exactly what the MultiArray<T, Cap> template below
// does -- a fixed-capacity count-prefixed array, the same pattern
// standard_msgs.hpp itself uses for PoseArray/Polygon/etc -- so this
// file both defines and exercises the whole family.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/std_msgs_pubsub.cpp -o std_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./std_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace std_msgs;

// --- std_msgs::*MultiArray family, user-defined (see file header) ---
// 8 elements is plenty to demonstrate the shape; raise kMultiArrayCap
// if your own use needs more, same tradeoff as any other fixed-cap
// array type in this codebase (PoseArray, Polygon, ...).
constexpr uint32_t kMultiArrayCap = 8;

#pragma pack(push, 1)
template <typename T, uint32_t Cap = kMultiArrayCap>
struct MultiArray {
    uint32_t count; // valid entries in data[0..count), <= Cap
    T data[Cap];
};
#pragma pack(pop)

using ByteMultiArray = MultiArray<uint8_t>;
using Int8MultiArray = MultiArray<int8_t>;
using Int16MultiArray = MultiArray<int16_t>;
using Int32MultiArray = MultiArray<int32_t>;
using Int64MultiArray = MultiArray<int64_t>;
using UInt8MultiArray = MultiArray<uint8_t>;
using UInt16MultiArray = MultiArray<uint16_t>;
using UInt32MultiArray = MultiArray<uint32_t>;
using UInt64MultiArray = MultiArray<uint64_t>;
using Float32MultiArray = MultiArray<float>;
using Float64MultiArray = MultiArray<double>;

// ByteMultiArray and UInt8MultiArray are the same instantiation
// (MultiArray<uint8_t>) -- ROS keeps them as distinct message types by
// name/semantics only, same as Byte and UInt8 themselves (wire.hpp).
// A fresh topic name still keeps them logically separate on the wire.

template <typename T, uint32_t Cap>
MultiArray<T, Cap> make_multiarray(int i) {
    MultiArray<T, Cap> m{};
    m.count = Cap;
    for (uint32_t k = 0; k < Cap; ++k) {
        m.data[k] = static_cast<T>(i + k);
    }
    return m;
}

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<Empty>("/relink/std/empty", [](const Empty&) {
        std::printf("std_msgs/Empty received\n");
    });
    node.subscribe<Time>("/relink/std/time", [](const Time& m) {
        std::printf("std_msgs/Time     sec=%u nsec=%u\n", m.sec, m.nsec);
    });
    node.subscribe<Duration>("/relink/std/duration", [](const Duration& m) {
        std::printf("std_msgs/Duration sec=%d nsec=%d\n", m.sec, m.nsec);
    });
    node.subscribe<ColorRGBA>("/relink/std/color", [](const ColorRGBA& m) {
        std::printf("std_msgs/ColorRGBA r=%.2f g=%.2f b=%.2f a=%.2f\n", m.r, m.g, m.b, m.a);
    });
    node.subscribe<Header>("/relink/std/header", [](const Header& m) {
        std::printf("std_msgs/Header   seq=%u frame=%s\n", m.seq, m.frame_id_str().c_str());
    });
    node.subscribe<String>("/relink/std/string", [](const String& m) {
        std::printf("std_msgs/String   \"%s\"\n", m.str().c_str());
    });

    node.subscribe<ByteMultiArray>("/relink/std/multiarray/byte", [](const ByteMultiArray& m) {
        std::printf("std_msgs/ByteMultiArray    count=%u [0]=%u\n", m.count, m.data[0]);
    });
    node.subscribe<Int8MultiArray>("/relink/std/multiarray/int8", [](const Int8MultiArray& m) {
        std::printf("std_msgs/Int8MultiArray    count=%u [0]=%d\n", m.count, m.data[0]);
    });
    node.subscribe<Int16MultiArray>("/relink/std/multiarray/int16", [](const Int16MultiArray& m) {
        std::printf("std_msgs/Int16MultiArray   count=%u [0]=%d\n", m.count, m.data[0]);
    });
    node.subscribe<Int32MultiArray>("/relink/std/multiarray/int32", [](const Int32MultiArray& m) {
        std::printf("std_msgs/Int32MultiArray   count=%u [0]=%d\n", m.count, m.data[0]);
    });
    node.subscribe<Int64MultiArray>("/relink/std/multiarray/int64", [](const Int64MultiArray& m) {
        std::printf("std_msgs/Int64MultiArray   count=%u [0]=%lld\n", m.count, static_cast<long long>(m.data[0]));
    });
    node.subscribe<UInt8MultiArray>("/relink/std/multiarray/uint8", [](const UInt8MultiArray& m) {
        std::printf("std_msgs/UInt8MultiArray   count=%u [0]=%u\n", m.count, m.data[0]);
    });
    node.subscribe<UInt16MultiArray>("/relink/std/multiarray/uint16", [](const UInt16MultiArray& m) {
        std::printf("std_msgs/UInt16MultiArray  count=%u [0]=%u\n", m.count, m.data[0]);
    });
    node.subscribe<UInt32MultiArray>("/relink/std/multiarray/uint32", [](const UInt32MultiArray& m) {
        std::printf("std_msgs/UInt32MultiArray  count=%u [0]=%u\n", m.count, m.data[0]);
    });
    node.subscribe<UInt64MultiArray>("/relink/std/multiarray/uint64", [](const UInt64MultiArray& m) {
        std::printf("std_msgs/UInt64MultiArray  count=%u [0]=%llu\n", m.count, static_cast<unsigned long long>(m.data[0]));
    });
    node.subscribe<Float32MultiArray>("/relink/std/multiarray/float32", [](const Float32MultiArray& m) {
        std::printf("std_msgs/Float32MultiArray count=%u [0]=%.2f\n", m.count, m.data[0]);
    });
    node.subscribe<Float64MultiArray>("/relink/std/multiarray/float64", [](const Float64MultiArray& m) {
        std::printf("std_msgs/Float64MultiArray count=%u [0]=%.2f\n", m.count, m.data[0]);
    });

    node.advertise<Empty>("/relink/std/empty");
    node.advertise<Time>("/relink/std/time");
    node.advertise<Duration>("/relink/std/duration");
    node.advertise<ColorRGBA>("/relink/std/color");
    node.advertise<Header>("/relink/std/header");
    node.advertise<String>("/relink/std/string");

    node.advertise<ByteMultiArray>("/relink/std/multiarray/byte");
    node.advertise<Int8MultiArray>("/relink/std/multiarray/int8");
    node.advertise<Int16MultiArray>("/relink/std/multiarray/int16");
    node.advertise<Int32MultiArray>("/relink/std/multiarray/int32");
    node.advertise<Int64MultiArray>("/relink/std/multiarray/int64");
    node.advertise<UInt8MultiArray>("/relink/std/multiarray/uint8");
    node.advertise<UInt16MultiArray>("/relink/std/multiarray/uint16");
    node.advertise<UInt32MultiArray>("/relink/std/multiarray/uint32");
    node.advertise<UInt64MultiArray>("/relink/std/multiarray/uint64");
    node.advertise<Float32MultiArray>("/relink/std/multiarray/float32");
    node.advertise<Float64MultiArray>("/relink/std/multiarray/float64");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        node.publish<Empty>("/relink/std/empty", Empty{});
        node.publish<Time>("/relink/std/time", Time::now());
        node.publish<Duration>("/relink/std/duration", Duration{ .sec = i, .nsec = 0 });
        node.publish<ColorRGBA>("/relink/std/color", ColorRGBA{ .r = 1.0f, .g = 0.5f, .b = 0.0f, .a = 1.0f });

        Header h{};
        h.seq = i;
        h.stamp_now();
        h.set_frame_id("base_link");
        node.publish<Header>("/relink/std/header", h);

        String s{};
        s.set("hello from std_msgs_pubsub");
        node.publish<String>("/relink/std/string", s);

        node.publish<ByteMultiArray>("/relink/std/multiarray/byte", make_multiarray<uint8_t, kMultiArrayCap>(i));
        node.publish<Int8MultiArray>("/relink/std/multiarray/int8", make_multiarray<int8_t, kMultiArrayCap>(i));
        node.publish<Int16MultiArray>("/relink/std/multiarray/int16", make_multiarray<int16_t, kMultiArrayCap>(i));
        node.publish<Int32MultiArray>("/relink/std/multiarray/int32", make_multiarray<int32_t, kMultiArrayCap>(i));
        node.publish<Int64MultiArray>("/relink/std/multiarray/int64", make_multiarray<int64_t, kMultiArrayCap>(i));
        node.publish<UInt8MultiArray>("/relink/std/multiarray/uint8", make_multiarray<uint8_t, kMultiArrayCap>(i));
        node.publish<UInt16MultiArray>("/relink/std/multiarray/uint16", make_multiarray<uint16_t, kMultiArrayCap>(i));
        node.publish<UInt32MultiArray>("/relink/std/multiarray/uint32", make_multiarray<uint32_t, kMultiArrayCap>(i));
        node.publish<UInt64MultiArray>("/relink/std/multiarray/uint64", make_multiarray<uint64_t, kMultiArrayCap>(i));
        node.publish<Float32MultiArray>("/relink/std/multiarray/float32", make_multiarray<float, kMultiArrayCap>(i));
        node.publish<Float64MultiArray>("/relink/std/multiarray/float64", make_multiarray<double, kMultiArrayCap>(i));

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}
