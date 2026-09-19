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

// Splits `entries` into groups each safe to pass to
// encode_topic_dir_announce()/encode_topic_dir_reply() in one packet --
// respecting BOTH kTopicDirMaxEntries (a count cap) AND
// kTopicDirMaxPacket (a byte-size cap), which are not automatically
// consistent with each other: kTopicDirMaxEntries=512 entries of
// real-length topic names (e.g. "/relink/stress/a2b_042") can easily
// encode to ~13-14KB, well over kTopicDirMaxPacket=8192 -- so chunking
// by kTopicDirMaxEntries alone can still hand encode_topic_dir_announce()
// a batch that decode_topic_dir_entries() on the other end rejects
// outright for being oversized. A caller with many entries and no idea
// how long the names are has no other safe way to chunk. Returns an
// empty vector for empty input (caller decides whether that still means
// "send one empty packet" or "send nothing").
inline std::vector<std::vector<TopicDirEntry>> chunk_topic_dir_entries(
    const std::vector<TopicDirEntry>& entries) {
    constexpr size_t kHeaderLen = kTopicDirMagicLen + sizeof(uint16_t);
    std::vector<std::vector<TopicDirEntry>> chunks;
    std::vector<TopicDirEntry> current;
    size_t current_len = kHeaderLen;
    for (const auto& e : entries) {
        size_t entry_len = sizeof(uint32_t) + 1 + e.name.size();
        if (!current.empty() &&
            (current.size() >= kTopicDirMaxEntries || current_len + entry_len > kTopicDirMaxPacket)) {
            chunks.push_back(std::move(current));
            current = std::vector<TopicDirEntry>();
            current_len = kHeaderLen;
        }
        current.push_back(e);
        current_len += entry_len;
    }
    if (!current.empty()) chunks.push_back(std::move(current));
    return chunks;
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

// --- Topic role directory: WHO (ip:port) is publishing/subscribing a
// topic, separate from the name directory above (which only ever
// carries a topic_id -> name mapping, never per-peer addresses or
// roles). Answers `rl_topic.py info`'s "show p2p connection details"
// need. See topic_directory.py's identical section for the full
// rationale (kept there since Python mirrors this byte-for-byte).
//
// Wire shape:
//   RLPA (role announce, node -> rlcore, periodic): magic(4) count(u16)
//     + count * { topic_id(u32) role(u8) }
//   RLPQ (role query, rl_topic -> rlcore): magic(4), nothing else
//   RLPR (role reply, rlcore -> rl_topic): magic(4) count(u16)
//     + count * { topic_id(u32) role(u8) ip(u32, host order) port(u16) }

inline constexpr char kRoleAnnounceMagic[kTopicDirMagicLen] = {'R', 'L', 'P', 'A'};
inline constexpr char kRoleQueryMagic[kTopicDirMagicLen]    = {'R', 'L', 'P', 'Q'};
inline constexpr char kRoleReplyMagic[kTopicDirMagicLen]    = {'R', 'L', 'P', 'R'};

inline constexpr uint8_t kRolePublisher = 1;
inline constexpr uint8_t kRoleSubscriber = 2;

inline constexpr size_t kRoleMaxEntries = 2048;
inline constexpr size_t kRoleMaxPacket = 8192;

enum class RoleDirKind { Announce, Query, Reply, Unknown };

inline RoleDirKind role_packet_kind(const uint8_t* buf, size_t len) {
    if (len < kTopicDirMagicLen) return RoleDirKind::Unknown;
    if (std::memcmp(buf, kRoleAnnounceMagic, kTopicDirMagicLen) == 0) return RoleDirKind::Announce;
    if (std::memcmp(buf, kRoleQueryMagic, kTopicDirMagicLen) == 0) return RoleDirKind::Query;
    if (std::memcmp(buf, kRoleReplyMagic, kTopicDirMagicLen) == 0) return RoleDirKind::Reply;
    return RoleDirKind::Unknown;
}

inline bool encode_role_query(uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (out_capacity < kTopicDirMagicLen) return false;
    std::memcpy(out, kRoleQueryMagic, kTopicDirMagicLen);
    *out_len = kTopicDirMagicLen;
    return true;
}

struct RoleAnnounceEntry {
    uint32_t topic_id;
    uint8_t role;
};

struct RolePeerEntry {
    uint32_t topic_id;
    uint8_t role;
    uint32_t ip;
    uint16_t port;
};

inline std::vector<std::vector<RoleAnnounceEntry>> chunk_role_announce_entries(
    const std::vector<RoleAnnounceEntry>& entries) {
    constexpr size_t kEntryLen = sizeof(uint32_t) + 1;
    constexpr size_t kHeaderLen = kTopicDirMagicLen + sizeof(uint16_t);
    std::vector<std::vector<RoleAnnounceEntry>> chunks;
    std::vector<RoleAnnounceEntry> current;
    size_t current_len = kHeaderLen;
    for (const auto& e : entries) {
        if (!current.empty() &&
            (current.size() >= kRoleMaxEntries || current_len + kEntryLen > kRoleMaxPacket)) {
            chunks.push_back(std::move(current));
            current = std::vector<RoleAnnounceEntry>();
            current_len = kHeaderLen;
        }
        current.push_back(e);
        current_len += kEntryLen;
    }
    if (!current.empty()) chunks.push_back(std::move(current));
    return chunks;
}

inline bool encode_role_announce(const std::vector<RoleAnnounceEntry>& entries,
                                  uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (entries.size() > kRoleMaxEntries) return false;
    size_t off = 0;
    if (out_capacity < kTopicDirMagicLen + sizeof(uint16_t)) return false;
    std::memcpy(out + off, kRoleAnnounceMagic, kTopicDirMagicLen);
    off += kTopicDirMagicLen;
    uint16_t count = static_cast<uint16_t>(entries.size());
    std::memcpy(out + off, &count, sizeof(count));
    off += sizeof(count);
    for (const auto& e : entries) {
        constexpr size_t kEntryLen = sizeof(uint32_t) + 1;
        if (off + kEntryLen > out_capacity) return false;
        std::memcpy(out + off, &e.topic_id, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        out[off++] = e.role;
    }
    *out_len = off;
    return true;
}

inline bool decode_role_announce(const uint8_t* buf, size_t len, std::vector<RoleAnnounceEntry>* out) {
    if (len < kTopicDirMagicLen + sizeof(uint16_t) || len > kRoleMaxPacket) return false;
    size_t off = kTopicDirMagicLen;
    uint16_t count;
    std::memcpy(&count, buf + off, sizeof(count));
    off += sizeof(count);
    if (count > kRoleMaxEntries) return false;
    std::vector<RoleAnnounceEntry> entries;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (off + sizeof(uint32_t) + 1 > len) return false;
        RoleAnnounceEntry e{};
        std::memcpy(&e.topic_id, buf + off, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        e.role = buf[off++];
        entries.push_back(e);
    }
    if (off != len) return false;
    *out = std::move(entries);
    return true;
}

inline std::vector<std::vector<RolePeerEntry>> chunk_role_peer_entries(
    const std::vector<RolePeerEntry>& entries) {
    constexpr size_t kEntryLen = sizeof(uint32_t) + 1 + sizeof(uint32_t) + sizeof(uint16_t);
    constexpr size_t kHeaderLen = kTopicDirMagicLen + sizeof(uint16_t);
    std::vector<std::vector<RolePeerEntry>> chunks;
    std::vector<RolePeerEntry> current;
    size_t current_len = kHeaderLen;
    for (const auto& e : entries) {
        if (!current.empty() &&
            (current.size() >= kRoleMaxEntries || current_len + kEntryLen > kRoleMaxPacket)) {
            chunks.push_back(std::move(current));
            current = std::vector<RolePeerEntry>();
            current_len = kHeaderLen;
        }
        current.push_back(e);
        current_len += kEntryLen;
    }
    if (!current.empty()) chunks.push_back(std::move(current));
    return chunks;
}

inline bool encode_role_reply(const std::vector<RolePeerEntry>& entries,
                               uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (entries.size() > kRoleMaxEntries) return false;
    size_t off = 0;
    if (out_capacity < kTopicDirMagicLen + sizeof(uint16_t)) return false;
    std::memcpy(out + off, kRoleReplyMagic, kTopicDirMagicLen);
    off += kTopicDirMagicLen;
    uint16_t count = static_cast<uint16_t>(entries.size());
    std::memcpy(out + off, &count, sizeof(count));
    off += sizeof(count);
    for (const auto& e : entries) {
        constexpr size_t kEntryLen = sizeof(uint32_t) + 1 + sizeof(uint32_t) + sizeof(uint16_t);
        if (off + kEntryLen > out_capacity) return false;
        std::memcpy(out + off, &e.topic_id, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        out[off++] = e.role;
        std::memcpy(out + off, &e.ip, sizeof(e.ip));
        off += sizeof(e.ip);
        std::memcpy(out + off, &e.port, sizeof(e.port));
        off += sizeof(e.port);
    }
    *out_len = off;
    return true;
}

inline bool decode_role_reply(const uint8_t* buf, size_t len, std::vector<RolePeerEntry>* out) {
    if (len < kTopicDirMagicLen + sizeof(uint16_t) || len > kRoleMaxPacket) return false;
    size_t off = kTopicDirMagicLen;
    uint16_t count;
    std::memcpy(&count, buf + off, sizeof(count));
    off += sizeof(count);
    if (count > kRoleMaxEntries) return false;
    std::vector<RolePeerEntry> entries;
    entries.reserve(count);
    constexpr size_t kEntryLen = sizeof(uint32_t) + 1 + sizeof(uint32_t) + sizeof(uint16_t);
    for (uint16_t i = 0; i < count; ++i) {
        if (off + kEntryLen > len) return false;
        RolePeerEntry e{};
        std::memcpy(&e.topic_id, buf + off, sizeof(e.topic_id));
        off += sizeof(e.topic_id);
        e.role = buf[off++];
        std::memcpy(&e.ip, buf + off, sizeof(e.ip));
        off += sizeof(e.ip);
        std::memcpy(&e.port, buf + off, sizeof(e.port));
        off += sizeof(e.port);
        entries.push_back(e);
    }
    if (off != len) return false;
    *out = std::move(entries);
    return true;
}

} // namespace relink
