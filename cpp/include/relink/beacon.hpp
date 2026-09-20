// Step 5 (part 1): BeaconPacket wire encode/decode — mode B (multicast)
// discovery protocol, per relink-com-spec.md "Mode B" section. Pure
// byte-level functions only, no sockets, mirroring register.hpp's split
// so the framing logic is independently unit-testable.
//
// Wire layout (little-endian, matches wire.hpp's BeaconPacketHeader):
//   BeaconPacket = BeaconPacketHeader(8B: node_ip, node_port, topic_count)
//                  + topic_count * uint32_t topic_ids
//                  + OPTIONAL 8B extension: uint32 node_pid, uint32 capability_flags
//
// The extension (Phase 5 of the local-IPC plan) lets a same-host
// shared-memory-capable peer be recognized -- see relink.hpp's
// advertise_local_ipc/subscribe_local_ipc. It's genuinely optional on
// decode (length check is ">=", not "=="): a beacon without it still
// decodes fine (pid/shm_capable default to 0/false), and a beacon WITH
// it still decodes fine on a decoder that predates this field, AS LONG
// AS that decoder was also written with ">=" -- the original "!="
// check would have rejected any beacon carrying this extension
// outright, so it had to be relaxed here rather than left as
// historical byte-count strictness.

#pragma once

#include "relink/wire.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace relink {

inline constexpr size_t kMaxBeaconTopics = 128; // sanity cap, fixed-size buffers
inline constexpr uint32_t kCapFlagShmLocalIpc = 0x01;

enum class BeaconEncodeResult { Ok, TooManyTopics, BufferTooSmall };
enum class BeaconDecodeResult { Ok, TooShort, LengthMismatch };

inline BeaconEncodeResult encode_beacon_packet(
    uint32_t node_ip, uint16_t node_port,
    const uint32_t* topic_ids, uint16_t topic_count,
    uint8_t* out, size_t out_capacity, size_t* out_len,
    uint32_t node_pid = 0, bool shm_capable = false) {
    if (topic_count > kMaxBeaconTopics) return BeaconEncodeResult::TooManyTopics;

    const size_t needed = sizeof(BeaconPacketHeader) + size_t(topic_count) * sizeof(uint32_t) + 8;
    if (out_capacity < needed) return BeaconEncodeResult::BufferTooSmall;

    BeaconPacketHeader hdr{};
    hdr.node_ip = node_ip;
    hdr.node_port = node_port;
    hdr.topic_count = topic_count;

    size_t off = 0;
    std::memcpy(out + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);
    for (uint16_t i = 0; i < topic_count; ++i) {
        uint32_t t = topic_ids[i];
        std::memcpy(out + off, &t, sizeof(t));
        off += sizeof(t);
    }
    uint32_t flags = shm_capable ? kCapFlagShmLocalIpc : 0;
    std::memcpy(out + off, &node_pid, sizeof(node_pid));
    off += sizeof(node_pid);
    std::memcpy(out + off, &flags, sizeof(flags));
    off += sizeof(flags);
    *out_len = off;
    return BeaconEncodeResult::Ok;
}

struct DecodedBeacon {
    uint32_t node_ip;
    uint16_t node_port;
    uint16_t topic_count;
    const uint8_t* topic_ids_raw;
    uint32_t node_pid = 0;
    bool shm_capable = false;
};

inline BeaconDecodeResult decode_beacon_packet(const uint8_t* buf, size_t len, DecodedBeacon* out) {
    if (len < sizeof(BeaconPacketHeader)) return BeaconDecodeResult::TooShort;
    BeaconPacketHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    const size_t needed = sizeof(hdr) + size_t(hdr.topic_count) * sizeof(uint32_t);
    if (len < needed) return BeaconDecodeResult::LengthMismatch;

    out->node_ip = hdr.node_ip;
    out->node_port = hdr.node_port;
    out->topic_count = hdr.topic_count;
    out->topic_ids_raw = buf + sizeof(hdr);
    out->node_pid = 0;
    out->shm_capable = false;
    if (len >= needed + 8) {
        uint32_t pid = 0, flags = 0;
        std::memcpy(&pid, buf + needed, sizeof(pid));
        std::memcpy(&flags, buf + needed + 4, sizeof(flags));
        out->node_pid = pid;
        out->shm_capable = (flags & kCapFlagShmLocalIpc) != 0;
    }
    return BeaconDecodeResult::Ok;
}

inline uint32_t beacon_topic_at(const DecodedBeacon& b, size_t i) {
    uint32_t v;
    std::memcpy(&v, b.topic_ids_raw + i * sizeof(uint32_t), sizeof(v));
    return v;
}

} // namespace relink
