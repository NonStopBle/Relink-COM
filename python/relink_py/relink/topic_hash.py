"""Deterministic string -> uint32 topic_id hashing, mirroring
relink/include/relink/topic_hash.hpp byte-for-byte (same FNV-1a
algorithm) so a topic name resolves to the same wire id in both
languages. See topic_hash.hpp's docstring for why this is a one-way
hash, not a reversible encoding -- "decoding" only works via each
node's own local name registry (RelinkNode._topic_id_for /
.topic_name_for), not by inverting the hash.
"""

_FNV_OFFSET_BASIS_32 = 0x811C9DC5
_FNV_PRIME_32 = 0x01000193
_MASK_32 = 0xFFFFFFFF


def fnv1a32(name: str) -> int:
    h = _FNV_OFFSET_BASIS_32
    for b in name.encode("utf-8"):
        h ^= b
        h = (h * _FNV_PRIME_32) & _MASK_32
    return h
