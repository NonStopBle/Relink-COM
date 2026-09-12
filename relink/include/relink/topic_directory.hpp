// Topic name directory protocol -- a small side-channel, separate from
// the data path and from discovery's peer-routing beacons/registration,
// used only by rl_topic (the CLI tool) and answered by any RelinkNode
// and by the rlcore daemon.
//
// Three packet kinds, all starting with a 4-byte magic tag so they can
// never be confused with a BeaconPacket, RegisterRequest, or a data
// frame sharing the same port:
//
//   RLNM (announce) -- broadcast/unicast by a node whenever it has
//                       resolved topic names, unprompted. Lets rlcore
//                       (and, in multicast mode, any other node) build
//                       up a table passively, the same way beacons build
//                       up the peer-routing table.
//   RLNQ (query)    -- sent by rl_topic to ask "what topic names do you
//                       know?" No payload beyond the magic.
//   RLNR (reply)    -- sent in response to RLNQ, same payload shape as
//                       RLNM: every {topic_id, name} pair the replier
//                       currently knows.
//
// Wire shape of RLNM/RLNR (RLNQ is just the 4-byte magic, nothing else):
//   magic       : 4 bytes ('R','L','N','M' or 'R','L','N','R')
//   entry_count : uint16_t
//   entries[entry_count]:
//     topic_id  : uint32_t
//     name_len  : uint8_t   (<= 255; real topic names are far shorter)
//     name      : name_len bytes, UTF-8, NOT null-terminated

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace relink {

inline constexpr size_t kTopicDirMagicLen = 4;
inline constexpr char kTopicDirAnnounceMagic[kTopicDirMagicLen] = {'R', 'L', 'N', 'M'};
inline constexpr char kTopicDirQueryMagic[kTopicDirMagicLen]    = {'R', 'L', 'N', 'Q'};
inline constexpr char kTopicDirReplyMagic[kTopicDirMagicLen]    = {'R', 'L', 'N', 'R'};

inline constexpr size_t kTopicDirMaxEntries = 512;   // sanity cap
inline constexpr size_t kTopicDirMaxNameLen = 255;
inline constexpr size_t kTopicDirMaxPacket = 8192;   // sanity cap for one datagram

struct TopicDirEntry {
    uint32_t topic_id;
    std::string name;
};

enum class TopicDirKind { Announce, Query, Reply, Unknown };

// Peeks at a buffer's first 4 bytes to classify it -- callers use this
// before attempting to decode a beacon/register packet arriving on the
// same socket, so an unrelated packet type is never misinterpreted.
inline TopicDirKind topic_dir_packet_kind(const uint8_t* buf, size_t len) {
    if (len < kTopicDirMagicLen) return TopicDirKind::Unknown;
    if (std::memcmp(buf, kTopicDirAnnounceMagic, kTopicDirMagicLen) == 0) return TopicDirKind::Announce;
    if (std::memcmp(buf, kTopicDirQueryMagic, kTopicDirMagicLen) == 0) return TopicDirKind::Query;
    if (std::memcmp(buf, kTopicDirReplyMagic, kTopicDirMagicLen) == 0) return TopicDirKind::Reply;
    return TopicDirKind::Unknown;
}

inline bool encode_topic_dir_query(uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (out_capacity < kTopicDirMagicLen) return false;
    std::memcpy(out, kTopicDirQueryMagic, kTopicDirMagicLen);
    *out_len = kTopicDirMagicLen;
    return true;
}

// Shared encoder for both Announce and Reply (identical payload shape).
inline bool encode_topic_dir_entries(const char magic[kTopicDirMagicLen],
                                      const std::vector<TopicDirEntry>& entries,
                                      uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (entries.size() > kTopicDirMaxEntries) return false;
    size_t off = 0;
    if (out_capacity < kTopicDirMagicLen + sizeof(uint16_t)) return false;
    std::memcpy(out + off, magic, kTopicDirMagicLen);
    off += kTopicDirMagicLen;
    uint16_t count = static_cast<uint16_t>(entries.size());
    std::memcpy(out + off, &count, sizeof(count));
    off += sizeof(count);

    for (const auto& e : entries) {
        if (e.name.size() > kTopicDirMaxNameLen) return false;
        size_t needed = sizeof(uint32_t) + 1 + e.name.size();
        if (off + needed > out_capacity) return false;
        std::memcpy(out + off, &e.topic_id, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        uint8_t name_len = static_cast<uint8_t>(e.name.size());
        out[off++] = name_len;
        std::memcpy(out + off, e.name.data(), name_len);
        off += name_len;
    }
    *out_len = off;
    return true;
}

inline bool encode_topic_dir_announce(const std::vector<TopicDirEntry>& entries,
                                       uint8_t* out, size_t out_capacity, size_t* out_len) {
    return encode_topic_dir_entries(kTopicDirAnnounceMagic, entries, out, out_capacity, out_len);
}

inline bool encode_topic_dir_reply(const std::vector<TopicDirEntry>& entries,
                                    uint8_t* out, size_t out_capacity, size_t* out_len) {
    return encode_topic_dir_entries(kTopicDirReplyMagic, entries, out, out_capacity, out_len);
}

// Decodes an Announce or Reply payload (caller already checked the magic
// via topic_dir_packet_kind). Returns false on any malformed/truncated
// packet -- never partially fills `out`.
inline bool decode_topic_dir_entries(const uint8_t* buf, size_t len, std::vector<TopicDirEntry>* out) {
    if (len < kTopicDirMagicLen + sizeof(uint16_t) || len > kTopicDirMaxPacket) return false;
    size_t off = kTopicDirMagicLen;
    uint16_t count;
    std::memcpy(&count, buf + off, sizeof(count));
    off += sizeof(count);
    if (count > kTopicDirMaxEntries) return false;

    std::vector<TopicDirEntry> entries;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (off + sizeof(uint32_t) + 1 > len) return false;
        TopicDirEntry e{};
        std::memcpy(&e.topic_id, buf + off, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        uint8_t name_len = buf[off++];
        if (off + name_len > len) return false;
        e.name.assign(reinterpret_cast<const char*>(buf + off), name_len);
        off += name_len;
        entries.push_back(std::move(e));
    }
    if (off != len) return false; // trailing garbage: reject rather than silently accept
    *out = std::move(entries);
    return true;
}

} // namespace relink
