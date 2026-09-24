// Step 4: RegisterRequest / RegisterAck wire encode/decode — mode A
// (rlcore) discovery protocol, per relink-com-spec.md "Mode A" section.
//
// Pure byte-level functions only (no sockets) so the exact same logic
// can be verified against a Python implementation byte-for-byte in
// tests, and so the rlcore daemon and the node-side client can share
// this header without pulling in socket code.
//
// Wire layout (all little-endian, matching wire.hpp's *Header structs):
//   RegisterRequest = RegisterRequestHeader(8B) + topic_count * uint32_t
//   RegisterAck     = RegisterAckHeader(3B) + peer_count * RegisterAckPeer(16B)

#pragma once

#include "relink/wire.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace relink {

inline constexpr uint16_t kRlCoreDefaultPort = 8445;
// The real limit is topic_count's own uint16_t range combined with fitting
// in one UDP/IPv4 datagram (65507 bytes max): (65507 - header) / 4 bytes
// per topic_id =~ 16374. The old value here (128) was an arbitrary, much
// lower sanity cap with no equivalent in the Python binding (register.py
// has no topic-count cap at all beyond the wire format's own limits) --
// at real large-system topic counts (e.g. ~1000 topics declared by one
// node under the default shared-socket/multiplex mode, all in one
// RegisterRequest) it silently made encode_register_request() return
// TooManyTopics, which register_with_rlcore_on_socket() then returned as
// a plain ok=false with NO error printed for that specific path -- so
// registration failed completely (zero peers, zero data delivery) while
// looking, from the caller's side, like rlcore was simply never
// reachable. Matches the buffer sizes in rlcore_client.hpp/udp_transport.hpp.
inline constexpr size_t kMaxRegisterTopics = 16374;

enum class RegisterEncodeResult { Ok, TooManyTopics, BufferTooSmall };
enum class RegisterDecodeResult { Ok, TooShort, LengthMismatch };

// --- RegisterRequest ---

inline RegisterEncodeResult encode_register_request(
    uint32_t node_ip, uint16_t node_port,
    const uint32_t* topic_ids, uint16_t topic_count,
    uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (topic_count > kMaxRegisterTopics) return RegisterEncodeResult::TooManyTopics;

    const size_t needed = sizeof(RegisterRequestHeader) + size_t(topic_count) * sizeof(uint32_t);
    if (out_capacity < needed) return RegisterEncodeResult::BufferTooSmall;

    RegisterRequestHeader hdr{};
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
    *out_len = off;
    return RegisterEncodeResult::Ok;
}

struct DecodedRegisterRequest {
    uint32_t node_ip;
    uint16_t node_port;
    uint16_t topic_count;
    const uint8_t* topic_ids_raw; // topic_count * uint32_t, little-endian, in-buffer
};

inline RegisterDecodeResult decode_register_request(const uint8_t* buf, size_t len,
                                                      DecodedRegisterRequest* out) {
    if (len < sizeof(RegisterRequestHeader)) return RegisterDecodeResult::TooShort;
    RegisterRequestHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    const size_t needed = sizeof(hdr) + size_t(hdr.topic_count) * sizeof(uint32_t);
    if (len != needed) return RegisterDecodeResult::LengthMismatch;

    out->node_ip = hdr.node_ip;
    out->node_port = hdr.node_port;
    out->topic_count = hdr.topic_count;
    out->topic_ids_raw = buf + sizeof(hdr);
    return RegisterDecodeResult::Ok;
}

inline uint32_t register_request_topic_at(const DecodedRegisterRequest& req, size_t i) {
    uint32_t v;
    std::memcpy(&v, req.topic_ids_raw + i * sizeof(uint32_t), sizeof(v));
    return v;
}

// --- RegisterAck ---

inline RegisterEncodeResult encode_register_ack(
    uint8_t status, const RegisterAckPeer* peers, uint16_t peer_count,
    uint8_t* out, size_t out_capacity, size_t* out_len) {
    const size_t needed = sizeof(RegisterAckHeader) + size_t(peer_count) * sizeof(RegisterAckPeer);
    if (out_capacity < needed) return RegisterEncodeResult::BufferTooSmall;

    RegisterAckHeader hdr{};
    hdr.status = status;
    hdr.peer_count = peer_count;

    size_t off = 0;
    std::memcpy(out + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);
    for (uint16_t i = 0; i < peer_count; ++i) {
        std::memcpy(out + off, &peers[i], sizeof(RegisterAckPeer));
        off += sizeof(RegisterAckPeer);
    }
    *out_len = off;
    return RegisterEncodeResult::Ok;
}

struct DecodedRegisterAck {
    uint8_t status;
    uint16_t peer_count;
    const uint8_t* peers_raw; // peer_count * RegisterAckPeer, in-buffer
};

inline RegisterDecodeResult decode_register_ack(const uint8_t* buf, size_t len,
                                                 DecodedRegisterAck* out) {
    if (len < sizeof(RegisterAckHeader)) return RegisterDecodeResult::TooShort;
    RegisterAckHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    const size_t needed = sizeof(hdr) + size_t(hdr.peer_count) * sizeof(RegisterAckPeer);
    if (len != needed) return RegisterDecodeResult::LengthMismatch;

    out->status = hdr.status;
    out->peer_count = hdr.peer_count;
    out->peers_raw = buf + sizeof(hdr);
    return RegisterDecodeResult::Ok;
}

inline RegisterAckPeer register_ack_peer_at(const DecodedRegisterAck& ack, size_t i) {
    RegisterAckPeer p{};
    std::memcpy(&p, ack.peers_raw + i * sizeof(RegisterAckPeer), sizeof(p));
    return p;
}

} // namespace relink
