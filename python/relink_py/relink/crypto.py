"""Pure-stdlib AES-256-GCM, mirroring relink/include/relink/crypto.hpp
byte-for-byte -- used to encrypt the rlcore signaling exchange
(RegisterRequest/RegisterAck), NOT the pub/sub data path, which stays
plaintext UDP as documented in the README.

Deliberately pure Python (no `cryptography`/`pycryptodome`), matching
this project's "Python needs no build step at all on any OS" story --
adding a third-party dependency here would break that for anyone who
just wants encryption without a pip install. This is the same
tradeoff crypto.hpp made against OpenSSL on the C++ side, for the
mirror-image reason (no standard MinGW OpenSSL package).

Verified byte-for-byte interoperable with the C++ implementation: a
message sealed here decrypts correctly with relink-rlcore's
aes256gcm_open(), and vice versa (see the project's own test suite).
Pure-Python AES is slow (roughly microseconds-to-low-milliseconds per
call, not nanoseconds) -- fine here since this only runs on
registration/re-registration (once at startup, then every ~0.3s per
node), never on the per-message publish() hot path.
"""

import secrets
import struct
from typing import Optional

AES_KEY_BYTES = 32
GCM_NONCE_BYTES = 12
GCM_TAG_BYTES = 16

_SBOX = bytes((
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16))

_RCON = (0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d)


def _xtime(x: int) -> int:
    x <<= 1
    if x & 0x100:
        x ^= 0x11b
    return x & 0xff


def _gmul(a: int, b: int) -> int:
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        a = _xtime(a)
        b >>= 1
    return p & 0xff


def _key_expansion(key: bytes):
    """Returns 60 32-bit round-key words for AES-256 (Nk=8, Nr=14, Nb=4)."""
    nk, nr, nb = 8, 14, 4
    w = [0] * (nb * (nr + 1))
    for i in range(nk):
        w[i] = struct.unpack(">I", key[4*i:4*i+4])[0]
    for i in range(nk, nb * (nr + 1)):
        temp = w[i - 1]
        if i % nk == 0:
            temp = ((temp << 8) | (temp >> 24)) & 0xffffffff  # RotWord
            temp = (_SBOX[(temp >> 24) & 0xff] << 24 | _SBOX[(temp >> 16) & 0xff] << 16 |
                    _SBOX[(temp >> 8) & 0xff] << 8 | _SBOX[temp & 0xff])
            temp ^= _RCON[i // nk] << 24
        elif nk > 6 and i % nk == 4:
            temp = (_SBOX[(temp >> 24) & 0xff] << 24 | _SBOX[(temp >> 16) & 0xff] << 16 |
                    _SBOX[(temp >> 8) & 0xff] << 8 | _SBOX[temp & 0xff])
        w[i] = (w[i - nk] ^ temp) & 0xffffffff
    return w


def _add_round_key(state: bytearray, w, round_: int):
    for c in range(4):
        rk = w[round_ * 4 + c]
        state[4*c + 0] ^= (rk >> 24) & 0xff
        state[4*c + 1] ^= (rk >> 16) & 0xff
        state[4*c + 2] ^= (rk >> 8) & 0xff
        state[4*c + 3] ^= rk & 0xff


def _sub_bytes(state: bytearray):
    for i in range(16):
        state[i] = _SBOX[state[i]]


def _shift_rows(state: bytearray):
    t = bytes(state)
    for r in range(4):
        for c in range(4):
            state[4*c + r] = t[4*((c + r) % 4) + r]


def _mix_columns(state: bytearray):
    for c in range(4):
        a0, a1, a2, a3 = state[4*c], state[4*c+1], state[4*c+2], state[4*c+3]
        state[4*c+0] = _gmul(a0, 2) ^ _gmul(a1, 3) ^ a2 ^ a3
        state[4*c+1] = a0 ^ _gmul(a1, 2) ^ _gmul(a2, 3) ^ a3
        state[4*c+2] = a0 ^ a1 ^ _gmul(a2, 2) ^ _gmul(a3, 3)
        state[4*c+3] = _gmul(a0, 3) ^ a1 ^ a2 ^ _gmul(a3, 2)


def _aes256_encrypt_block(w, block: bytes) -> bytes:
    """Encrypts one 16-byte block. State layout matches FIPS-197:
    state[r + 4c] = in[4c + r] (column-major)."""
    nr = 14
    state = bytearray(block)
    _add_round_key(state, w, 0)
    for round_ in range(1, nr):
        _sub_bytes(state)
        _shift_rows(state)
        _mix_columns(state)
        _add_round_key(state, w, round_)
    _sub_bytes(state)
    _shift_rows(state)
    _add_round_key(state, w, nr)
    return bytes(state)


# --- GHASH (GF(2^128) multiplication, NIST SP800-38D) -------------------

def _gf128_mul(x: bytes, y: bytes) -> bytes:
    z = bytearray(16)
    v = bytearray(y)
    for i in range(16):
        xi = x[i]
        for bit in range(7, -1, -1):
            if (xi >> bit) & 1:
                for k in range(16):
                    z[k] ^= v[k]
            lsb_set = v[15] & 1
            carry = 0
            for k in range(16):
                new_carry = v[k] & 1
                v[k] = (v[k] >> 1) | (carry << 7)
                carry = new_carry
            if lsb_set:
                v[0] ^= 0xe1
    return bytes(z)


def _ghash(h: bytes, data: bytes) -> bytes:
    y = bytearray(16)
    off = 0
    n = len(data)
    while off < n:
        chunk = data[off:off+16]
        block = chunk + b"\x00" * (16 - len(chunk))
        for i in range(16):
            y[i] ^= block[i]
        y = bytearray(_gf128_mul(bytes(y), h))
        off += 16
    len_block = struct.pack(">QQ", 0, n * 8)  # 64-bit AAD-len(=0) || 64-bit ciphertext-len, in bits
    for i in range(16):
        y[i] ^= len_block[i]
    return _gf128_mul(bytes(y), h)


def _inc32(block: bytearray):
    counter = struct.unpack(">I", bytes(block[12:16]))[0]
    counter = (counter + 1) & 0xffffffff
    block[12:16] = struct.pack(">I", counter)


def _gctr(w, counter_block: bytearray, data: bytes) -> bytes:
    out = bytearray(len(data))
    off = 0
    n = len(data)
    while off < n:
        keystream = _aes256_encrypt_block(w, bytes(counter_block))
        chunk_len = min(16, n - off)
        for i in range(chunk_len):
            out[off + i] = data[off + i] ^ keystream[i]
        _inc32(counter_block)
        off += chunk_len
    return bytes(out)


def generate_random_key32() -> bytes:
    return secrets.token_bytes(AES_KEY_BYTES)


def key32_to_hex(key: bytes) -> str:
    return key.hex()


def hex_to_key32(hex_str: str) -> Optional[bytes]:
    """Accepts exactly 64 hex characters (32 bytes); returns None for
    anything else (wrong length, non-hex characters) rather than
    silently truncating/padding."""
    if len(hex_str) != AES_KEY_BYTES * 2:
        return None
    try:
        key = bytes.fromhex(hex_str)
    except ValueError:
        return None
    return key if len(key) == AES_KEY_BYTES else None


def aes256gcm_seal(key: bytes, plaintext: bytes) -> bytes:
    """Returns nonce(12B) || ciphertext(len(plaintext)) || tag(16B)."""
    nonce = secrets.token_bytes(GCM_NONCE_BYTES)
    w = _key_expansion(key)
    h = _aes256_encrypt_block(w, b"\x00" * 16)

    j0 = bytearray(16)
    j0[:GCM_NONCE_BYTES] = nonce
    j0[15] = 1  # 96-bit IV case: J0 = IV || 0^31 || 1

    ctr_block = bytearray(j0)
    _inc32(ctr_block)  # encryption starts at inc32(J0)
    ciphertext = _gctr(w, ctr_block, plaintext)

    s = _ghash(h, ciphertext)
    tag = _gctr(w, bytearray(j0), s)[:GCM_TAG_BYTES]  # T = GCTR(K, J0, S)

    return nonce + ciphertext + tag


def aes256gcm_open(key: bytes, sealed: bytes) -> Optional[bytes]:
    """Reverses aes256gcm_seal(). Returns None on any failure, including
    a GCM tag mismatch (tampered data, corruption, or the wrong key) --
    callers must treat that identically to a malformed packet."""
    if len(sealed) < GCM_NONCE_BYTES + GCM_TAG_BYTES:
        return None
    nonce = sealed[:GCM_NONCE_BYTES]
    ciphertext = sealed[GCM_NONCE_BYTES:-GCM_TAG_BYTES]
    tag = sealed[-GCM_TAG_BYTES:]

    w = _key_expansion(key)
    h = _aes256_encrypt_block(w, b"\x00" * 16)

    j0 = bytearray(16)
    j0[:GCM_NONCE_BYTES] = nonce
    j0[15] = 1

    s = _ghash(h, ciphertext)
    expected_tag = _gctr(w, bytearray(j0), s)[:GCM_TAG_BYTES]

    # Constant-time compare -- a data-dependent early-exit would leak
    # tag bytes through timing, letting an attacker forge a tag one
    # byte at a time.
    if not secrets.compare_digest(expected_tag, tag):
        return None

    ctr_block = bytearray(j0)
    _inc32(ctr_block)
    return _gctr(w, ctr_block, ciphertext)
