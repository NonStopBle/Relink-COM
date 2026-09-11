// Step 1 unit tests: byte-exact wire layout for RelinkHeader, BeaconPacket
// prefix, RegisterRequest/Ack prefixes, default types, and MultiArray
// header. No test framework dependency — plain asserts, exits nonzero on
// failure so it's easy to run under tmux/CI.

#include "relink/wire.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace relink;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } else { \
        std::printf("ok: %s\n", #cond); \
    } \
} while (0)

int main() {
    // --- sizes: no padding introduced anywhere ---
    CHECK(sizeof(RelinkHeader) == 7);
    CHECK(sizeof(SecureExt) == 8);
    CHECK(sizeof(RegisterRequestHeader) == 8);
    CHECK(sizeof(RegisterAckHeader) == 3);
    CHECK(sizeof(RegisterAckPeer) == 8);
    CHECK(sizeof(BeaconPacketHeader) == 8);
    CHECK(sizeof(MultiArrayHeader) == 4);

    CHECK(sizeof(Bool) == 1);
    CHECK(sizeof(Byte) == 1);
    CHECK(sizeof(Char) == 1);
    CHECK(sizeof(Int8) == 1);
    CHECK(sizeof(Int16) == 2);
    CHECK(sizeof(Int32) == 4);
    CHECK(sizeof(Int64) == 8);
    CHECK(sizeof(UInt8) == 1);
    CHECK(sizeof(UInt16) == 2);
    CHECK(sizeof(UInt32) == 4);
    CHECK(sizeof(UInt64) == 8);
    CHECK(sizeof(Float32) == 4);
    CHECK(sizeof(Float64) == 8);

    // --- trivially copyable, as required by the custom-type rule ---
    CHECK(is_wire_type_v<RelinkHeader>);
    CHECK(is_wire_type_v<Int32>);

    // --- field offsets match the documented field order exactly ---
    RelinkHeader h{};
    CHECK(offsetof(RelinkHeader, topic_id) == 0);
    CHECK(offsetof(RelinkHeader, seq_num) == 2);
    CHECK(offsetof(RelinkHeader, payload_len) == 4);
    CHECK(offsetof(RelinkHeader, flags) == 6);

    // --- raw byte layout check: build a header, inspect its bytes ---
    // topic_id=0x1234, seq_num=0x5678, payload_len=0x0009, flags=0x03
    h.topic_id = 0x1234;
    h.seq_num = 0x5678;
    h.payload_len = 0x0009;
    h.flags = 0x03;

    uint8_t raw[7];
    std::memcpy(raw, &h, sizeof(h));

    // On a little-endian host (x86/x64/ARM default), the in-memory byte
    // order of a packed struct already matches the little-endian wire
    // format documented in the spec, so memcpy is a valid "encode" step
    // without any byte-swapping needed on these platforms.
    CHECK(raw[0] == 0x34); CHECK(raw[1] == 0x12); // topic_id LE
    CHECK(raw[2] == 0x78); CHECK(raw[3] == 0x56); // seq_num LE
    CHECK(raw[4] == 0x09); CHECK(raw[5] == 0x00); // payload_len LE
    CHECK(raw[6] == 0x03);                        // flags

    // --- flags bit meaning ---
    CHECK(kFlagSecure == 0x01);
    CHECK(kFlagChecksum == 0x02);
    CHECK((h.flags & kFlagSecure) != 0);
    CHECK((h.flags & kFlagChecksum) != 0);

    // --- start/stop bytes ---
    CHECK(kStartByte == '#');
    CHECK(kStopByte == '\n');

    // --- BeaconPacketHeader field offsets ---
    CHECK(offsetof(BeaconPacketHeader, node_ip) == 0);
    CHECK(offsetof(BeaconPacketHeader, node_port) == 4);
    CHECK(offsetof(BeaconPacketHeader, topic_count) == 6);

    // --- RegisterRequestHeader field offsets ---
    CHECK(offsetof(RegisterRequestHeader, node_ip) == 0);
    CHECK(offsetof(RegisterRequestHeader, node_port) == 4);
    CHECK(offsetof(RegisterRequestHeader, topic_count) == 6);

    // --- RegisterAckPeer field offsets ---
    CHECK(offsetof(RegisterAckPeer, ip) == 0);
    CHECK(offsetof(RegisterAckPeer, port) == 4);
    CHECK(offsetof(RegisterAckPeer, topic_id) == 6);

    // --- default type field name/offset (all wrap a single `data`) ---
    CHECK(offsetof(Float32, data) == 0);
    CHECK(offsetof(Int64, data) == 0);

    // --- MTU sanity: full plaintext frame overhead is small ---
    // '#'(1) + header(7) + '\n'(1) = 9 bytes overhead, no secure/checksum
    constexpr size_t kPlaintextOverhead = 1 + sizeof(RelinkHeader) + 1;
    CHECK(kPlaintextOverhead == 9);

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    } else {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
}
