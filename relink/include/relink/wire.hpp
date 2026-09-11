// ReLink wire format — step 1 of relink_com_implement.md
//
// Every struct here defines a language-independent byte layout (see
// relink-com-spec.md "Multi-language support"). All integers are
// little-endian (true on every mainstream target we build for: x86/x64,
// ARM in its default little-endian mode). All structs are byte-packed
// (#pragma pack(push, 1)) so there is no compiler-inserted padding
// anywhere in a wire struct.
//
// This header only defines byte layouts and pure helpers (encode/decode,
// type traits). It does not implement sockets, threads, or the ring
// buffer — those are later build-order steps.

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace relink {

// ---------------------------------------------------------------------
// Frame delimiters
// ---------------------------------------------------------------------
inline constexpr uint8_t kStartByte = 0x23; // '#'
inline constexpr uint8_t kStopByte  = 0x0A; // '\n'

// ---------------------------------------------------------------------
// flags bits
// ---------------------------------------------------------------------
inline constexpr uint8_t kFlagSecure   = 0x01; // bit 0
inline constexpr uint8_t kFlagChecksum = 0x02; // bit 1

// ---------------------------------------------------------------------
// RelinkHeader — 7 bytes on the wire, immediately after the start byte.
//   topic_id    : uint16_t, little-endian
//   seq_num     : uint16_t, little-endian
//   payload_len : uint16_t, little-endian — bytes of payload only, not
//                 counting SecureExt/auth tag/checksum
//   flags       : uint8_t  — bit0 secure, bit1 checksum, bits 2-7 reserved
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct RelinkHeader {
    uint16_t topic_id;
    uint16_t seq_num;
    uint16_t payload_len;
    uint8_t  flags;
};
#pragma pack(pop)
static_assert(sizeof(RelinkHeader) == 7, "RelinkHeader must be exactly 7 bytes on the wire");
static_assert(std::is_trivially_copyable<RelinkHeader>::value, "RelinkHeader must be trivially copyable");

// ---------------------------------------------------------------------
// SecureExt — 8 bytes, present immediately after RelinkHeader only when
// kFlagSecure is set. Deferred (secure=true) past v1 core per spec, but
// the layout is fixed now so the wire format never needs to change.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct SecureExt {
    uint32_t session_id;
    uint32_t nonce_counter;
};
#pragma pack(pop)
static_assert(sizeof(SecureExt) == 8, "SecureExt must be exactly 8 bytes on the wire");
static_assert(std::is_trivially_copyable<SecureExt>::value, "SecureExt must be trivially copyable");

inline constexpr size_t kAuthTagBytes = 16; // GCM auth tag, present iff secure
inline constexpr size_t kChecksumBytes = 2; // CRC16, present iff checksum flag set

// ---------------------------------------------------------------------
// RegisterRequest / RegisterAck — mode A (com-core) discovery packets.
// Variable-length (topic_ids[]/peers[] are trailing arrays), so these are
// documented as fixed prefixes here; the wire encoder appends the
// trailing arrays manually rather than relying on a flexible array
// member (not standard-portable in a header meant to be read as spec).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct RegisterRequestHeader {
    uint32_t node_ip;      // network-order-free: stored host-endian uint32,
                            // wire encoding below is little-endian like everything else
    uint16_t node_port;
    uint16_t topic_count;
    // followed by topic_count * uint16_t topic_ids
};
#pragma pack(pop)
static_assert(sizeof(RegisterRequestHeader) == 8, "RegisterRequestHeader must be exactly 8 bytes");

#pragma pack(push, 1)
struct RegisterAckHeader {
    uint8_t  status;       // 0 = ok, 1 = error
    uint16_t peer_count;
    // followed by peer_count * RegisterAckPeer
};
#pragma pack(pop)
static_assert(sizeof(RegisterAckHeader) == 3, "RegisterAckHeader must be exactly 3 bytes");

#pragma pack(push, 1)
struct RegisterAckPeer {
    uint32_t ip;
    uint16_t port;
    uint16_t topic_id;
};
#pragma pack(pop)
static_assert(sizeof(RegisterAckPeer) == 8, "RegisterAckPeer must be exactly 8 bytes");

// ---------------------------------------------------------------------
// BeaconPacket — mode B (multicast) discovery packet, fixed prefix.
//   node_ip      : uint32_t
//   node_port    : uint16_t
//   topic_count  : uint16_t
//   followed by topic_count * uint16_t topic_ids
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct BeaconPacketHeader {
    uint32_t node_ip;
    uint16_t node_port;
    uint16_t topic_count;
};
#pragma pack(pop)
static_assert(sizeof(BeaconPacketHeader) == 8, "BeaconPacketHeader must be exactly 8 bytes");

// ---------------------------------------------------------------------
// Default primitive types (std_msgs naming) — each wraps a single `data`
// field, packed with no padding.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct Bool    { uint8_t  data; }; // 0/1, stored as a byte on the wire
struct Byte    { uint8_t  data; };
struct Char    { uint8_t  data; };
struct Int8    { int8_t   data; };
struct Int16   { int16_t  data; };
struct Int32   { int32_t  data; };
struct Int64   { int64_t  data; };
struct UInt8   { uint8_t  data; };
struct UInt16  { uint16_t data; };
struct UInt32  { uint32_t data; };
struct UInt64  { uint64_t data; };
struct Float32 { float    data; };
struct Float64 { double   data; };
#pragma pack(pop)

static_assert(sizeof(Bool) == 1, "Bool must be 1 byte");
static_assert(sizeof(Int64) == 8, "Int64 must be 8 bytes");
static_assert(sizeof(Float32) == 4, "Float32 must be 4 bytes");
static_assert(sizeof(Float64) == 8, "Float64 must be 8 bytes");

// ---------------------------------------------------------------------
// MultiArray<T> — length-prefixed array wire layout:
//   count : uint32_t
//   data  : T[count], tightly packed, no padding between elements
//
// This template describes the *layout* only. Because C++ doesn't allow
// a standard, portable flexible array member cleanly, the actual
// serialize/deserialize helpers (used by the ring buffer / UDP steps)
// build/read this layout manually via MultiArrayHeader + a byte pointer,
// rather than instantiating MultiArray<T> objects directly.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct MultiArrayHeader {
    uint32_t count;
    // followed by count * T, tightly packed
};
#pragma pack(pop)
static_assert(sizeof(MultiArrayHeader) == 4, "MultiArrayHeader must be exactly 4 bytes");

// ---------------------------------------------------------------------
// Custom type rules (enforced at compile time by advertise/subscribe/
// publish in later steps, expressed here as a reusable trait):
//   - trivially copyable
//   - not one of the types that themselves contain a MultiArray (no
//     nested dynamic-length types) -- checked structurally where needed,
//     not encoded generically here since C++ has no reflection.
// ---------------------------------------------------------------------
template <typename T>
inline constexpr bool is_wire_type_v = std::is_trivially_copyable<T>::value;

} // namespace relink
