// Deterministic string -> uint32_t topic_id hashing, so library users can
// write human-readable topic names ("/relink/camera/front") instead of
// hand-assigning numeric ids, while the wire format still only carries 4
// bytes per RelinkHeader.topic_id.
//
// This is a one-way hash (FNV-1a, 32-bit), NOT a reversible encoding --
// packing an arbitrary-length string losslessly into 32 bits is
// information-theoretically impossible past ~6 characters. "Decoding" a
// topic_id back to its name is done via RelinkNode's local name registry
// (populated from this process's own advertise/subscribe/publish calls),
// not by inverting the hash.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace relink {

// FNV-1a 32-bit: fast, well-distributed, no dependencies. Same algorithm
// must be used on the Python side (relink_py/relink/topic_hash.py) for
// cross-language topics to resolve to the same id.
inline constexpr uint32_t fnv1a32(const char* data, size_t len) {
    uint32_t hash = 0x811c9dc5u;          // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 0x01000193u;               // FNV prime
    }
    return hash;
}

inline uint32_t fnv1a32(const std::string& s) {
    return fnv1a32(s.data(), s.size());
}

} // namespace relink
