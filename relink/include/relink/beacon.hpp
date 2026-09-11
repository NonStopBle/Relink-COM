// Step 5 (part 1): BeaconPacket wire encode/decode — mode B (multicast)
// discovery protocol, per relink-com-spec.md "Mode B" section. Pure
// byte-level functions only, no sockets, mirroring register.hpp's split
// so the framing logic is independently unit-testable.
//
// Wire layout (little-endian, matches wire.hpp's BeaconPacketHeader):
//   BeaconPacket = BeaconPacketHeader(8B: node_ip, node_port, topic_count)
//                  + topic_count * uint16_t topic_ids

#pragma once

#include "relink/wire.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace relink {

inline constexpr size_t kMaxBeaconTopics = 128; // sanity cap, fixed-size buffers

enum class BeaconEncodeResult { Ok, TooManyTopics, BufferTooSmall };
enum class BeaconDecodeResult { Ok, TooShort, LengthMismatch };

inline BeaconEncodeResult encode_beacon_packet(
    uint32_t node_ip, uint16_t node_port,
    const uint16_t* topic_ids, uint16_t topic_count,
    uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (topic_count > kMaxBeaconTopics) return BeaconEncodeResult::TooManyTopics;

    const size_t needed = sizeof(BeaconPacketHeader) + size_t(topic_count) * sizeof(uint16_t);
    if (out_capacity < needed) return BeaconEncodeResult::BufferTooSmall;

    BeaconPacketHeader hdr{};
    hdr.node_ip = node_ip;
    hdr.node_port = node_port;
    hdr.topic_count = topic_count;

    size_t off = 0;
    std::memcpy(out + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);
    for (uint16_t i = 0; i < topic_count; ++i) {
        uint16_t t = topic_ids[i];
        std::memcpy(out + off, &t, sizeof(t));
        off += sizeof(t);
    }
    *out_len = off;
    return BeaconEncodeResult::Ok;
}

struct DecodedBeacon {
    uint32_t node_ip;
    uint16_t node_port;
    uint16_t topic_count;
    const uint8_t* topic_ids_raw;
};

inline BeaconDecodeResult decode_beacon_packet(const uint8_t* buf, size_t len, DecodedBeacon* out) {
    if (len < sizeof(BeaconPacketHeader)) return BeaconDecodeResult::TooShort;
    BeaconPacketHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    const size_t needed = sizeof(hdr) + size_t(hdr.topic_count) * sizeof(uint16_t);
    if (len != needed) return BeaconDecodeResult::LengthMismatch;

    out->node_ip = hdr.node_ip;
    out->node_port = hdr.node_port;
    out->topic_count = hdr.topic_count;
    out->topic_ids_raw = buf + sizeof(hdr);
    return BeaconDecodeResult::Ok;
}

inline uint16_t beacon_topic_at(const DecodedBeacon& b, size_t i) {
    uint16_t v;
    std::memcpy(&v, b.topic_ids_raw + i * sizeof(uint16_t), sizeof(v));
    return v;
}

} // namespace relink
