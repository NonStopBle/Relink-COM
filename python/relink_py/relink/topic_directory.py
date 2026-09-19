"""Topic name directory protocol -- Python mirror of topic_directory.hpp.
See that file for the full protocol rationale/wire shape. Small
side-channel, separate from the data path and from discovery's own
peer-routing beacons, used only by rl_topic.py and answered by any
RelinkNode and by the rlcore daemon."""
import struct
from dataclasses import dataclass
from typing import List, Optional, Tuple

MAGIC_ANNOUNCE = b"RLNM"
MAGIC_QUERY = b"RLNQ"
MAGIC_REPLY = b"RLNR"

MAX_ENTRIES = 512
MAX_NAME_LEN = 255
MAX_PACKET = 8192


@dataclass
class TopicDirEntry:
    topic_id: int
    name: str


def packet_kind(data: bytes) -> str:
    """Returns 'announce', 'query', 'reply', or 'unknown'."""
    if len(data) < 4:
        return "unknown"
    head = data[:4]
    if head == MAGIC_ANNOUNCE:
        return "announce"
    if head == MAGIC_QUERY:
        return "query"
    if head == MAGIC_REPLY:
        return "reply"
    return "unknown"


def encode_query() -> bytes:
    return MAGIC_QUERY


def _encode_entries(magic: bytes, entries: List[TopicDirEntry]) -> bytes:
    if len(entries) > MAX_ENTRIES:
        raise ValueError("too many topic directory entries")
    out = bytearray()
    out += magic
    out += struct.pack("<H", len(entries))
    for e in entries:
        name_bytes = e.name.encode("utf-8")
        if len(name_bytes) > MAX_NAME_LEN:
            raise ValueError(f"topic name too long for directory protocol: {e.name!r}")
        out += struct.pack("<IB", e.topic_id, len(name_bytes))
        out += name_bytes
    return bytes(out)


def encode_announce(entries: List[TopicDirEntry]) -> bytes:
    return _encode_entries(MAGIC_ANNOUNCE, entries)


def encode_reply(entries: List[TopicDirEntry]) -> bytes:
    return _encode_entries(MAGIC_REPLY, entries)


def chunk_entries(entries: List[TopicDirEntry]) -> List[List[TopicDirEntry]]:
    """Splits `entries` into groups each safe to pass to encode_announce()/
    encode_reply() in one packet -- respecting BOTH MAX_ENTRIES (a count
    cap) AND MAX_PACKET (a byte-size cap), which are not automatically
    consistent with each other: MAX_ENTRIES=512 entries of real-length
    topic names (e.g. "/relink/stress/a2b_042") can easily encode to
    ~13-14KB, well over MAX_PACKET=8192 -- so chunking by MAX_ENTRIES
    alone (the original, wrong approach) could still hand
    encode_announce() a batch that decode_entries() on the other end
    rejects outright for being oversized, even though it round-tripped
    through encode_announce() itself without error (encode_* only
    enforces MAX_ENTRIES and per-name length, never the aggregate
    MAX_PACKET size). A caller with N entries and no idea how long their
    names are has no other safe way to chunk. Returns [] for empty
    input (caller decides whether that still means "send one empty
    packet" or "send nothing")."""
    header_len = 6  # magic(4) + count(2), same for announce and reply
    chunks: List[List[TopicDirEntry]] = []
    current: List[TopicDirEntry] = []
    current_len = header_len
    for e in entries:
        entry_len = 5 + len(e.name.encode("utf-8"))  # topic_id(4)+name_len(1)+name
        if current and (len(current) >= MAX_ENTRIES or current_len + entry_len > MAX_PACKET):
            chunks.append(current)
            current = []
            current_len = header_len
        current.append(e)
        current_len += entry_len
    if current:
        chunks.append(current)
    return chunks


def decode_entries(data: bytes) -> Optional[List[TopicDirEntry]]:
    """Decodes an Announce or Reply payload (caller already checked the
    magic via packet_kind()). Returns None on any malformed/truncated
    packet -- never returns a partial list."""
    if len(data) < 6 or len(data) > MAX_PACKET:
        return None
    count = struct.unpack_from("<H", data, 4)[0]
    if count > MAX_ENTRIES:
        return None
    off = 6
    entries: List[TopicDirEntry] = []
    for _ in range(count):
        if off + 5 > len(data):
            return None
        topic_id, name_len = struct.unpack_from("<IB", data, off)
        off += 5
        if off + name_len > len(data):
            return None
        name = data[off:off + name_len].decode("utf-8", errors="replace")
        off += name_len
        entries.append(TopicDirEntry(topic_id, name))
    if off != len(data):
        return None  # trailing garbage: reject rather than silently accept
    return entries


# --- Topic role directory: WHO (ip:port) is publishing/subscribing a
# topic, separate from the name directory above (which only ever carries
# a topic_id -> name mapping, never per-peer addresses or roles).
# Answers `rl_topic.py info`'s "show p2p connection details" need.
#
# ROLE_ANNOUNCE is sent periodically by every node (piggybacked on the
# same cadence as rlcore re-registration, see node.py's
# _rlcore_reregister_loop()) so rlcore can expire a peer's role the same
# way it expires stale registrations (see relink_rlcore.py's
# REGISTRATION_TTL_S/prune_stale()) -- a one-time announce would leave
# rl_topic showing pub/sub roles for nodes that already exited.
#
# ROLE_QUERY/ROLE_REPLY mirror QUERY/REPLY above: rl_topic asks rlcore
# once, rlcore dumps its whole live role table (chunked the same way),
# and rl_topic filters client-side to the one topic it's asking about --
# consistent with how the name directory already works, and avoids
# needing a "query by topic id" wire message just for this.

MAGIC_ROLE_ANNOUNCE = b"RLPA"
MAGIC_ROLE_QUERY = b"RLPQ"
MAGIC_ROLE_REPLY = b"RLPR"

ROLE_PUBLISHER = 1
ROLE_SUBSCRIBER = 2

MAX_ROLE_ENTRIES = 2048
MAX_ROLE_PACKET = 8192

_ROLE_ANNOUNCE_ENTRY_FMT = "<IB"       # topic_id, role -- what a node sends
_ROLE_ANNOUNCE_ENTRY_LEN = struct.calcsize(_ROLE_ANNOUNCE_ENTRY_FMT)
_ROLE_PEER_ENTRY_FMT = "<IBIH"         # topic_id, role, ip, port -- what rlcore replies
_ROLE_PEER_ENTRY_LEN = struct.calcsize(_ROLE_PEER_ENTRY_FMT)


@dataclass
class RoleAnnounceEntry:
    topic_id: int
    role: int  # ROLE_PUBLISHER | ROLE_SUBSCRIBER (bitmask)


@dataclass
class RolePeerEntry:
    topic_id: int
    role: int
    ip: int    # host order, same convention as RegisterAckPeer
    port: int


def role_packet_kind(data: bytes) -> str:
    """Returns 'role_announce', 'role_query', 'role_reply', or 'unknown'."""
    if len(data) < 4:
        return "unknown"
    head = data[:4]
    if head == MAGIC_ROLE_ANNOUNCE:
        return "role_announce"
    if head == MAGIC_ROLE_QUERY:
        return "role_query"
    if head == MAGIC_ROLE_REPLY:
        return "role_reply"
    return "unknown"


def encode_role_query() -> bytes:
    return MAGIC_ROLE_QUERY


def chunk_role_announce_entries(entries: List[RoleAnnounceEntry]) -> List[List[RoleAnnounceEntry]]:
    header_len = 6
    chunks: List[List[RoleAnnounceEntry]] = []
    current: List[RoleAnnounceEntry] = []
    current_len = header_len
    for e in entries:
        if current and (len(current) >= MAX_ROLE_ENTRIES or
                         current_len + _ROLE_ANNOUNCE_ENTRY_LEN > MAX_ROLE_PACKET):
            chunks.append(current)
            current = []
            current_len = header_len
        current.append(e)
        current_len += _ROLE_ANNOUNCE_ENTRY_LEN
    if current:
        chunks.append(current)
    return chunks


def encode_role_announce(entries: List[RoleAnnounceEntry]) -> bytes:
    if len(entries) > MAX_ROLE_ENTRIES:
        raise ValueError("too many role announce entries")
    out = bytearray(MAGIC_ROLE_ANNOUNCE)
    out += struct.pack("<H", len(entries))
    for e in entries:
        out += struct.pack(_ROLE_ANNOUNCE_ENTRY_FMT, e.topic_id, e.role)
    return bytes(out)


def decode_role_announce(data: bytes) -> Optional[List[RoleAnnounceEntry]]:
    if len(data) < 6 or len(data) > MAX_ROLE_PACKET:
        return None
    count = struct.unpack_from("<H", data, 4)[0]
    needed = 6 + count * _ROLE_ANNOUNCE_ENTRY_LEN
    if count > MAX_ROLE_ENTRIES or len(data) != needed:
        return None
    entries = []
    off = 6
    for _ in range(count):
        topic_id, role = struct.unpack_from(_ROLE_ANNOUNCE_ENTRY_FMT, data, off)
        entries.append(RoleAnnounceEntry(topic_id, role))
        off += _ROLE_ANNOUNCE_ENTRY_LEN
    return entries


def chunk_role_peer_entries(entries: List[RolePeerEntry]) -> List[List[RolePeerEntry]]:
    header_len = 6
    chunks: List[List[RolePeerEntry]] = []
    current: List[RolePeerEntry] = []
    current_len = header_len
    for e in entries:
        if current and (len(current) >= MAX_ROLE_ENTRIES or
                         current_len + _ROLE_PEER_ENTRY_LEN > MAX_ROLE_PACKET):
            chunks.append(current)
            current = []
            current_len = header_len
        current.append(e)
        current_len += _ROLE_PEER_ENTRY_LEN
    if current:
        chunks.append(current)
    return chunks


def encode_role_reply(entries: List[RolePeerEntry]) -> bytes:
    if len(entries) > MAX_ROLE_ENTRIES:
        raise ValueError("too many role peer entries")
    out = bytearray(MAGIC_ROLE_REPLY)
    out += struct.pack("<H", len(entries))
    for e in entries:
        out += struct.pack(_ROLE_PEER_ENTRY_FMT, e.topic_id, e.role, e.ip, e.port)
    return bytes(out)


def decode_role_reply(data: bytes) -> Optional[List[RolePeerEntry]]:
    if len(data) < 6 or len(data) > MAX_ROLE_PACKET:
        return None
    count = struct.unpack_from("<H", data, 4)[0]
    needed = 6 + count * _ROLE_PEER_ENTRY_LEN
    if count > MAX_ROLE_ENTRIES or len(data) != needed:
        return None
    entries = []
    off = 6
    for _ in range(count):
        topic_id, role, ip, port = struct.unpack_from(_ROLE_PEER_ENTRY_FMT, data, off)
        entries.append(RolePeerEntry(topic_id, role, ip, port))
        off += _ROLE_PEER_ENTRY_LEN
    return entries
