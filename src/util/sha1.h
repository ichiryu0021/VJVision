// Minimal SHA-1 implementation (public-domain / RFC 3174).
// Used only for fingerprint hashing: SHA1("freq1|freq2|tdelta")[0:20 hex chars].
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vj {

inline std::array<uint32_t, 5> sha1(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // padded message: original + 0x80 + zeros + 64-bit big-endian length (bits)
    size_t bit_len = len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(total, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i)
        msg[total - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);

    auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[off + i * 4]) << 24) |
                   (uint32_t(msg[off + i * 4 + 1]) << 16) |
                   (uint32_t(msg[off + i * 4 + 2]) << 8) |
                   (uint32_t(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    return {h0, h1, h2, h3, h4};
}

// Return first `hex_chars` hex characters of the SHA1 digest (big-endian).
inline std::string sha1_hex_prefix(const void* data, size_t len, int hex_chars = 20) {
    auto h = sha1(reinterpret_cast<const uint8_t*>(data), len);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(hex_chars);
    uint8_t bytes[20];
    for (int i = 0; i < 5; ++i) {
        bytes[i * 4]     = (h[i] >> 24) & 0xFF;
        bytes[i * 4 + 1] = (h[i] >> 16) & 0xFF;
        bytes[i * 4 + 2] = (h[i] >> 8) & 0xFF;
        bytes[i * 4 + 3] = h[i] & 0xFF;
    }
    for (int i = 0; i < hex_chars; ++i)
        out.push_back(hex[(bytes[i / 2] >> (4 * (1 - (i & 1)))) & 0xF]);
    return out;
}

} // namespace vj
