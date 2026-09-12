// Relay fallback: for when direct peer-to-peer UDP hole punching cannot
// cross a NAT/firewall at all (verified against a real mobile-carrier
// NAT: a packet capture showed the carrier drops unsolicited inbound
// UDP structurally, not due to a timing race -- see README Step 13).
// A relay works there because both clients only ever open a NAT mapping
// toward the RELAY's one fixed (ip, port), never toward each other --
// the relay's replies always come from that same fixed remote endpoint,
// which is exactly the case every stateful NAT/firewall allows back in.
//
// Wire format is deliberately minimal: a REGISTER control packet (this
// file) to join a topic's forwarding group, and ordinary ReLink data
// frames (wire.hpp's RelinkHeader) for everything else -- the relay
// forwards those RAW AND UNMODIFIED, peeking only at the topic_id
// already sitting in the header, so a forwarded frame decodes on the
// receiving end exactly like a direct one (same seq_num, same
// topic_id). No second encoding, no extra copy beyond the one recvfrom
// already did.
#pragma once

#include "relink/wire.hpp"
#include "relink/frame.hpp"
#include <cstdint>
#include <cstring>

namespace relink {

inline constexpr uint16_t kRelayDefaultPort = 8446;

// First byte of a relay control packet. Chosen distinct from
// kStartByte (0x23, a real data frame) so the relay can tell the two
// apart with a single byte compare.
inline constexpr uint8_t kRelayControlMagic = 0x52; // 'R'
inline constexpr uint8_t kRelayRegister = 0x01;

// REGISTER packet: [magic][type][topic_id LE]. Idempotent -- also
// used as a keepalive, resent periodically by the client for as long
// as relay fallback is enabled for that topic.
#pragma pack(push, 1)
struct RelayRegisterPacket {
    uint8_t magic;
    uint8_t type;
    uint32_t topic_id;
};
#pragma pack(pop)
static_assert(sizeof(RelayRegisterPacket) == 6, "RelayRegisterPacket must be exactly 6 bytes on the wire");

inline size_t encode_relay_register(uint32_t topic_id, uint8_t* out, size_t out_cap) {
    if (out_cap < 6) return 0;
    out[0] = kRelayControlMagic;
    out[1] = kRelayRegister;
    uint32_t t = topic_id;
    std::memcpy(out + 2, &t, 4); // little-endian on all targets we build for
    return 6;
}

// Returns true and sets *topic_id if `buf` is a valid REGISTER packet.
inline bool decode_relay_register(const uint8_t* buf, size_t len, uint32_t* topic_id) {
    if (len != 6 || buf[0] != kRelayControlMagic || buf[1] != kRelayRegister) return false;
    std::memcpy(topic_id, buf + 2, 4);
    return true;
}

// Peeks the topic_id out of an ordinary ReLink data frame without
// fully decoding/validating it (the relay does not need to, and must
// not, understand payload contents -- it forwards bytes, not messages).
// Returns true and sets *topic_id if `buf` looks like a data frame
// (starts with kStartByte and is at least long enough to hold the
// header).
inline bool peek_frame_topic_id(const uint8_t* buf, size_t len, uint32_t* topic_id) {
    if (len < 1 + sizeof(RelinkHeader) || buf[0] != kStartByte) return false;
    std::memcpy(topic_id, buf + 1, 4); // RelinkHeader::topic_id is the header's first field
    return true;
}

} // namespace relink
