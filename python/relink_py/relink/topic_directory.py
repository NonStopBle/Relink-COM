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
