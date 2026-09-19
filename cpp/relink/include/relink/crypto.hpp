// Self-contained AES-256-GCM for encrypting the rlcore signaling
// exchange (RegisterRequest/RegisterAck) -- NOT the pub/sub data path,
// which stays plaintext UDP as documented elsewhere. A pre-shared
// 32-byte key (generated with `relink-rlcore --generate-key`, then
// given to both the daemon via --encrypt-key and to nodes via
// node.set_rlcore.setEncryptKey(...)) authenticates and encrypts the
// registration handshake so it can't be read or spoofed by anyone else
// on the same network segment.
//
// Deliberately dependency-free (no OpenSSL/libcrypto) rather than
// linking a system crypto library: this project's whole C++ core is a
// self-contained header set that cross-compiles for Windows with
// nothing but MinGW + -lws2_32 (see platform.hpp) -- OpenSSL has no
// standard MinGW-w64 package, so linking it here would silently break
// that Windows build for anyone including this header. AES-256-GCM is
// a fully specified, non-proprietary algorithm (FIPS-197 + NIST
// SP800-38D), implemented below straight from those specs and checked
// for interop against OpenSSL's implementation during development.
//
// Wire shape of a sealed message: [12-byte random nonce][ciphertext,
// same length as the plaintext][16-byte GCM tag].

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <stdexcept>
#include <chrono>
#include <random>

namespace relink {

inline constexpr size_t kAesKeyBytes = 32;
inline constexpr size_t kAesGcmNonceBytes = 12;
inline constexpr size_t kAesGcmTagBytes = 16;

// --- AES-256 block cipher (FIPS-197) -----------------------------------

namespace detail {

inline const uint8_t kSbox[256] = {
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
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

inline const uint8_t kRcon[15] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d};

// AES-256: Nk=8 (32-byte key), Nr=14 rounds, Nb=4 -> 60 round-key words.
struct Aes256 {
    uint32_t round_keys[60];

    explicit Aes256(const uint8_t key[kAesKeyBytes]) {
        constexpr int Nk = 8, Nr = 14, Nb = 4;
        uint32_t* w = round_keys;
        for (int i = 0; i < Nk; ++i) {
            w[i] = (uint32_t(key[4*i]) << 24) | (uint32_t(key[4*i+1]) << 16) |
                   (uint32_t(key[4*i+2]) << 8) | uint32_t(key[4*i+3]);
        }
        for (int i = Nk; i < Nb * (Nr + 1); ++i) {
            uint32_t temp = w[i - 1];
            if (i % Nk == 0) {
                temp = (temp << 8) | (temp >> 24); // RotWord
                temp = (uint32_t(kSbox[(temp >> 24) & 0xff]) << 24) |
                       (uint32_t(kSbox[(temp >> 16) & 0xff]) << 16) |
                       (uint32_t(kSbox[(temp >> 8) & 0xff]) << 8) |
                       uint32_t(kSbox[temp & 0xff]);
                temp ^= uint32_t(kRcon[i / Nk]) << 24;
            } else if (Nk > 6 && i % Nk == 4) {
                temp = (uint32_t(kSbox[(temp >> 24) & 0xff]) << 24) |
                       (uint32_t(kSbox[(temp >> 16) & 0xff]) << 16) |
                       (uint32_t(kSbox[(temp >> 8) & 0xff]) << 8) |
                       uint32_t(kSbox[temp & 0xff]);
            }
            w[i] = w[i - Nk] ^ temp;
        }
    }

    static uint8_t xtime(uint8_t x) {
        return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
    }
    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) p ^= a;
            a = xtime(a);
            b >>= 1;
        }
        return p;
    }

    // Encrypts one 16-byte block in place (state laid out column-major,
    // matching FIPS-197's state array: state[r + 4c] = in[4c + r]).
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        constexpr int Nr = 14;

        auto add_round_key = [&](int round) {
            for (int c = 0; c < 4; ++c) {
                uint32_t rk = round_keys[round * 4 + c];
                s[4*c + 0] ^= uint8_t(rk >> 24);
                s[4*c + 1] ^= uint8_t(rk >> 16);
                s[4*c + 2] ^= uint8_t(rk >> 8);
                s[4*c + 3] ^= uint8_t(rk);
            }
        };
        auto sub_bytes = [&]() { for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]]; };
        auto shift_rows = [&]() {
            uint8_t t[16];
            std::memcpy(t, s, 16);
            // row r, column c is at t[4*c + r]; ShiftRows rotates row r left by r.
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    s[4*c + r] = t[4*((c + r) % 4) + r];
        };
        auto mix_columns = [&]() {
            for (int c = 0; c < 4; ++c) {
                uint8_t a0 = s[4*c+0], a1 = s[4*c+1], a2 = s[4*c+2], a3 = s[4*c+3];
                s[4*c+0] = gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3;
                s[4*c+1] = a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3;
                s[4*c+2] = a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3);
                s[4*c+3] = gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2);
            }
        };

        add_round_key(0);
        for (int round = 1; round < Nr; ++round) {
            sub_bytes();
            shift_rows();
            mix_columns();
            add_round_key(round);
        }
        sub_bytes();
        shift_rows();
        add_round_key(Nr);

        std::memcpy(out, s, 16);
    }
};

// --- GHASH (GF(2^128) multiplication, NIST SP800-38D) ------------------

inline void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    std::memcpy(v, y, 16);
    for (int i = 0; i < 16; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            if ((x[i] >> bit) & 1) {
                for (int k = 0; k < 16; ++k) z[k] ^= v[k];
            }
            bool lsb_set = v[15] & 1;
            // right shift v by 1 (128-bit, MSB-first byte order)
            for (int k = 15; k > 0; --k) v[k] = (v[k] >> 1) | ((v[k-1] & 1) << 7);
            v[0] >>= 1;
            if (lsb_set) v[0] ^= 0xe1; // R = 11100001 || 0^120
        }
    }
    std::memcpy(out, z, 16);
}

// Computes GHASH_H over ciphertext only (no AAD, matching this
// project's use -- the whole registration payload is confidential, so
// there's no separate associated data to authenticate-but-not-encrypt).
inline void ghash(const uint8_t h[16], const uint8_t* data, size_t len, uint8_t out[16]) {
    uint8_t y[16] = {0};
    size_t off = 0;
    uint8_t block[16];
    while (off < len) {
        size_t n = (len - off < 16) ? (len - off) : 16;
        std::memset(block, 0, 16);
        std::memcpy(block, data + off, n);
        for (int i = 0; i < 16; ++i) y[i] ^= block[i];
        uint8_t tmp[16];
        gf128_mul(y, h, tmp);
        std::memcpy(y, tmp, 16);
        off += n;
    }
    // final block: 64-bit len(AAD)=0 || 64-bit len(C) in bits, big-endian
    uint8_t len_block[16] = {0};
    uint64_t c_bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) len_block[15 - i] = uint8_t(c_bits >> (8 * i));
    for (int i = 0; i < 16; ++i) y[i] ^= len_block[i];
    uint8_t tmp[16];
    gf128_mul(y, h, tmp);
    std::memcpy(out, tmp, 16);
}

inline void inc32(uint8_t block[16]) {
    // increments the last 4 bytes as a big-endian 32-bit counter
    for (int i = 15; i >= 12; --i) {
        if (++block[i] != 0) break;
    }
}

// CTR-mode keystream XOR, counter starting at `counter_block` (already
// incremented past J0 by the caller for the ciphertext pass).
inline void gctr(const Aes256& aes, uint8_t counter_block[16],
                  const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t keystream[16];
    size_t off = 0;
    while (off < len) {
        aes.encrypt_block(counter_block, keystream);
        size_t n = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ keystream[i];
        inc32(counter_block);
        off += n;
    }
}

} // namespace detail

inline void generate_random_key32(uint8_t key[kAesKeyBytes]) {
    // std::random_device, seeded further by the current time -- not a
    // hardware CSPRNG, but this only runs once per operator invocation
    // of `relink-rlcore --generate-key`, on demand, to produce a key a
    // human then copies to both sides; it is not used per-message or
    // per-connection, so it doesn't need to be fast, just unpredictable
    // enough that two operators never generate the same key by chance.
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < kAesKeyBytes; ++i) key[i] = static_cast<uint8_t>(dist(gen));
}

inline std::string key32_to_hex(const uint8_t key[kAesKeyBytes]) {
    static const char* hex_chars = "0123456789abcdef";
    std::string out;
    out.reserve(kAesKeyBytes * 2);
    for (size_t i = 0; i < kAesKeyBytes; ++i) {
        out.push_back(hex_chars[key[i] >> 4]);
        out.push_back(hex_chars[key[i] & 0x0f]);
    }
    return out;
}

// Accepts exactly 64 hex characters (32 bytes); anything else (wrong
// length, non-hex characters) is rejected rather than silently
// truncated/padded, since a short/garbled key here would otherwise
// fail in a way that looks like a network problem, not a config typo.
inline bool hex_to_key32(const std::string& hex, uint8_t key[kAesKeyBytes]) {
    if (hex.size() != kAesKeyBytes * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < kAesKeyBytes; ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Seals `pt` (pt_len bytes) with AES-256-GCM under `key`, writing
// nonce||ciphertext||tag to `out`. Returns false only if out_capacity
// is too small.
inline bool aes256gcm_seal(const uint8_t key[kAesKeyBytes],
                            const uint8_t* pt, size_t pt_len,
                            uint8_t* out, size_t out_capacity, size_t* out_len) {
    const size_t needed = kAesGcmNonceBytes + pt_len + kAesGcmTagBytes;
    if (out_capacity < needed) return false;

    uint8_t* nonce = out;
    uint8_t* ct = out + kAesGcmNonceBytes;
    uint8_t* tag = ct + pt_len;

    // Same CSPRNG source as generate_random_key32(), just fewer bytes.
    {
        std::random_device rd;
        std::mt19937_64 gen(rd() ^ static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t i = 0; i < kAesGcmNonceBytes; ++i) nonce[i] = static_cast<uint8_t>(dist(gen));
    }

    detail::Aes256 aes(key);
    uint8_t h[16] = {0};
    uint8_t zero[16] = {0};
    aes.encrypt_block(zero, h);

    uint8_t j0[16] = {0};
    std::memcpy(j0, nonce, kAesGcmNonceBytes);
    j0[15] = 1; // 96-bit IV case: J0 = IV || 0^31 || 1

    uint8_t ctr_block[16];
    std::memcpy(ctr_block, j0, 16);
    detail::inc32(ctr_block); // encryption starts at inc32(J0)
    detail::gctr(aes, ctr_block, pt, pt_len, ct);

    uint8_t s[16];
    detail::ghash(h, ct, pt_len, s);
    uint8_t tag_block[16];
    uint8_t j0_copy[16];
    std::memcpy(j0_copy, j0, 16);
    detail::gctr(aes, j0_copy, s, 16, tag_block); // T = GCTR(K, J0, S)
    std::memcpy(tag, tag_block, kAesGcmTagBytes);

    *out_len = needed;
    return true;
}

// Reverses aes256gcm_seal(). Returns false on any failure, including a
// GCM tag mismatch (tampered data, corruption, or the wrong key) --
// callers must treat that identically to a malformed packet, never
// fall back to using partially-decrypted output.
inline bool aes256gcm_open(const uint8_t key[kAesKeyBytes],
                            const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_capacity, size_t* out_len) {
    if (in_len < kAesGcmNonceBytes + kAesGcmTagBytes) return false;
    const uint8_t* nonce = in;
    const uint8_t* ct = in + kAesGcmNonceBytes;
    const size_t ct_len = in_len - kAesGcmNonceBytes - kAesGcmTagBytes;
    const uint8_t* tag = ct + ct_len;
    if (out_capacity < ct_len) return false;

    detail::Aes256 aes(key);
    uint8_t h[16] = {0};
    uint8_t zero[16] = {0};
    aes.encrypt_block(zero, h);

    uint8_t j0[16] = {0};
    std::memcpy(j0, nonce, kAesGcmNonceBytes);
    j0[15] = 1;

    uint8_t s[16];
    detail::ghash(h, ct, ct_len, s);
    uint8_t expected_tag[16];
    uint8_t j0_copy[16];
    std::memcpy(j0_copy, j0, 16);
    detail::gctr(aes, j0_copy, s, 16, expected_tag);

    // Constant-time compare -- a data-dependent early-exit here would
    // leak tag bytes through timing, letting an attacker forge a valid
    // tag one byte at a time.
    uint8_t diff = 0;
    for (size_t i = 0; i < kAesGcmTagBytes; ++i) diff |= expected_tag[i] ^ tag[i];
    if (diff != 0) return false;

    uint8_t ctr_block[16];
    std::memcpy(ctr_block, j0, 16);
    detail::inc32(ctr_block);
    detail::gctr(aes, ctr_block, ct, ct_len, out);
    *out_len = ct_len;
    return true;
}

} // namespace relink
