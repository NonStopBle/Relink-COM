// builtin_types_pubsub -- publishes and subscribes every built-in
// message type ReLink ships (relink/include/relink/wire.hpp), one
// topic per type, so this file doubles as a runnable reference list.
// These are the `std_msgs`-style default types available with no
// definition of your own needed -- see custom_types_pubsub.cpp for
// user-defined types instead.
//
// Built-in types (13 total), each wrapping a single `data` field:
//   Bool     UInt8
//   Byte     UInt16
//   Char     UInt32
//   Int8     UInt64
//   Int16    Float32
//   Int32    Float64
//   Int64
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/builtin_types_pubsub.cpp -o builtin_types_pubsub
// Run (in two terminals, or on two machines):
//   ./builtin_types_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    // One topic per built-in type. Subscribe before advertising, so an
    // early message from a peer that started first is never missed.
    node.subscribe<Bool>("/relink/types/bool", [](const Bool& m) {
        std::printf("Bool     %d\n", m.data);
    });
    node.subscribe<Byte>("/relink/types/byte", [](const Byte& m) {
        std::printf("Byte     %u\n", m.data);
    });
    node.subscribe<Char>("/relink/types/char", [](const Char& m) {
        std::printf("Char     '%c'\n", static_cast<char>(m.data));
    });
    node.subscribe<Int8>("/relink/types/int8", [](const Int8& m) {
        std::printf("Int8     %d\n", m.data);
    });
    node.subscribe<Int16>("/relink/types/int16", [](const Int16& m) {
        std::printf("Int16    %d\n", m.data);
    });
    node.subscribe<Int32>("/relink/types/int32", [](const Int32& m) {
        std::printf("Int32    %d\n", m.data);
    });
    node.subscribe<Int64>("/relink/types/int64", [](const Int64& m) {
        std::printf("Int64    %lld\n", static_cast<long long>(m.data));
    });
    node.subscribe<UInt8>("/relink/types/uint8", [](const UInt8& m) {
        std::printf("UInt8    %u\n", m.data);
    });
    node.subscribe<UInt16>("/relink/types/uint16", [](const UInt16& m) {
        std::printf("UInt16   %u\n", m.data);
    });
    node.subscribe<UInt32>("/relink/types/uint32", [](const UInt32& m) {
        std::printf("UInt32   %u\n", m.data);
    });
    node.subscribe<UInt64>("/relink/types/uint64", [](const UInt64& m) {
        std::printf("UInt64   %llu\n", static_cast<unsigned long long>(m.data));
    });
    node.subscribe<Float32>("/relink/types/float32", [](const Float32& m) {
        std::printf("Float32  %.3f\n", m.data);
    });
    node.subscribe<Float64>("/relink/types/float64", [](const Float64& m) {
        std::printf("Float64  %.6f\n", m.data);
    });

    node.advertise<Bool>("/relink/types/bool");
    node.advertise<Byte>("/relink/types/byte");
    node.advertise<Char>("/relink/types/char");
    node.advertise<Int8>("/relink/types/int8");
    node.advertise<Int16>("/relink/types/int16");
    node.advertise<Int32>("/relink/types/int32");
    node.advertise<Int64>("/relink/types/int64");
    node.advertise<UInt8>("/relink/types/uint8");
    node.advertise<UInt16>("/relink/types/uint16");
    node.advertise<UInt32>("/relink/types/uint32");
    node.advertise<UInt64>("/relink/types/uint64");
    node.advertise<Float32>("/relink/types/float32");
    node.advertise<Float64>("/relink/types/float64");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        node.publish<Bool>("/relink/types/bool", Bool{ .data = static_cast<uint8_t>(i % 2) });
        node.publish<Byte>("/relink/types/byte", Byte{ .data = static_cast<uint8_t>(i) });
        node.publish<Char>("/relink/types/char", Char{ .data = static_cast<uint8_t>('A' + (i % 26)) });
        node.publish<Int8>("/relink/types/int8", Int8{ .data = static_cast<int8_t>(-i) });
        node.publish<Int16>("/relink/types/int16", Int16{ .data = static_cast<int16_t>(-i) });
        node.publish<Int32>("/relink/types/int32", Int32{ .data = -i });
        node.publish<Int64>("/relink/types/int64", Int64{ .data = -static_cast<int64_t>(i) });
        node.publish<UInt8>("/relink/types/uint8", UInt8{ .data = static_cast<uint8_t>(i) });
        node.publish<UInt16>("/relink/types/uint16", UInt16{ .data = static_cast<uint16_t>(i) });
        node.publish<UInt32>("/relink/types/uint32", UInt32{ .data = static_cast<uint32_t>(i) });
        node.publish<UInt64>("/relink/types/uint64", UInt64{ .data = static_cast<uint64_t>(i) });
        node.publish<Float32>("/relink/types/float32", Float32{ .data = i * 0.5f });
        node.publish<Float64>("/relink/types/float64", Float64{ .data = i * 0.25 });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}
