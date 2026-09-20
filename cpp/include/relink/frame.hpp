// Step 3 (part 1): pure frame encode/decode helpers — no sockets, no
// threads. Kept separate from udp_transport.hpp so the byte-level framing
// logic (the part that must exactly match relink-com-spec.md's "Wire
// format" section) can be unit tested without any network I/O.
//
// v1 scope for this step: secure=false, checksum=false only (per spec
// step 3 note). SecureExt/auth-tag/CRC16 offsets are deferred; the sizes
// are already reserved in wire.hpp for when that lands.

#pragma once

#include "relink/wire.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>

namespace relink {

// Safe UDP payload budget: typical Ethernet MTU (1500) minus IP+UDP
// headers (28) minus ReLink's own frame overhead, with margin. Per spec:
// "roughly 1400-1450 bytes -- leave margin, don't assume the full 1500."
inline constexpr size_t kMaxPayloadBytes = 1400;

// Max full frame size a single encode can ever produce at this budget:
// '#' + header(9) + payload(<=1400) + '\n', no secure/checksum in v1.
inline constexpr size_t kMaxFrameBytes = 1 + sizeof(RelinkHeader) + kMaxPayloadBytes + 1;

enum class EncodeResult {
    Ok,
    PayloadTooLarge,
};

enum class DecodeResult {
    Ok,
    TooShort,       // buffer smaller than the minimum possible frame
    BadStartByte,
    BadStopByte,    // stop byte not found at the computed fixed offset
    LengthMismatch, // payload_len implies a frame longer than the buffer given
};

struct DecodedFrame {
    RelinkHeader header;
    const uint8_t* payload; // points into the input buffer, not owned/copied
    size_t payload_len;
};

// Encode a plaintext (secure=false, checksum=false) frame into `out`.
// `out` must be at least kMaxFrameBytes long. On success, *out_len is set
// to the number of bytes actually written. Rejects (does not truncate or
// silently fragment) any payload larger than kMaxPayloadBytes, per spec:
// "reject with a clear error if the message is too large."
inline EncodeResult encode_frame(uint32_t topic_id, uint16_t seq_num,
                                  const void* payload, size_t payload_len,
                                  uint8_t* out, size_t out_capacity,
                                  size_t* out_len) {
    if (payload_len > kMaxPayloadBytes) {
        return EncodeResult::PayloadTooLarge;
    }
    const size_t frame_len = 1 + sizeof(RelinkHeader) + payload_len + 1;
    if (out_capacity < frame_len) {
        return EncodeResult::PayloadTooLarge; // capacity too small to hold it
    }

    RelinkHeader header{};
    header.topic_id = topic_id;
    header.seq_num = seq_num;
    header.payload_len = static_cast<uint16_t>(payload_len);
    header.flags = 0; // secure=false, checksum=false in this step

    size_t off = 0;
    out[off++] = kStartByte;
    std::memcpy(out + off, &header, sizeof(header));
    off += sizeof(header);
    if (payload_len > 0) {
        std::memcpy(out + off, payload, payload_len);
        off += payload_len;
    }
    out[off++] = kStopByte;

    *out_len = off;
    return EncodeResult::Ok;
}

// Decode a received buffer in place (no copy of the payload). Per spec:
// "the receiver must never scan for '\n' to find the end of the frame"
// -- the stop-byte offset is computed directly from payload_len and
// flags, then checked, rather than searched for.
//
// v1 scope: only handles flags == 0 (secure=false, checksum=false)
// frames; any other flag bits are rejected as unsupported for now
// (SecureExt/checksum paths land when those features are implemented).
inline DecodeResult decode_frame(const uint8_t* buf, size_t len, DecodedFrame* out) {
    constexpr size_t kMinFrame = 1 + sizeof(RelinkHeader) + 1; // empty payload
    if (len < kMinFrame) {
        return DecodeResult::TooShort;
    }
    if (buf[0] != kStartByte) {
        return DecodeResult::BadStartByte;
    }

    RelinkHeader header{};
    std::memcpy(&header, buf + 1, sizeof(header));

    if (header.flags != 0) {
        // secure/checksum frames unsupported at this build-order step.
        return DecodeResult::LengthMismatch;
    }

    const size_t payload_off = 1 + sizeof(RelinkHeader);
    const size_t stop_off = payload_off + header.payload_len;
    if (stop_off >= len) {
        // stop_off must be a valid index within buf (the stop byte itself)
        return DecodeResult::LengthMismatch;
    }
    if (stop_off + 1 != len) {
        // buffer must contain exactly one frame, no trailing garbage
        return DecodeResult::LengthMismatch;
    }
    if (buf[stop_off] != kStopByte) {
        return DecodeResult::BadStopByte;
    }

    out->header = header;
    out->payload = buf + payload_off;
    out->payload_len = header.payload_len;
    return DecodeResult::Ok;
}

} // namespace relink
