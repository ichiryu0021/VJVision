#include "tags.h"
#include "path_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vj {

namespace {

// ---- small byte helpers -------------------------------------------------

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
uint32_t le32(const uint8_t* p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}
uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) |
           ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7)  |
            (uint32_t)(p[3] & 0x7f);
}

// Decode a Latin1-tagged ID3 text field (enc==0). Many Chinese/Japanese
// MP3s abuse the Latin1 encoding marker to store GBK/CP932/Shift-JIS bytes.
// Try CP936 (GBK) first, then CP932 (Shift-JIS); fall back to a strict
// Latin1→UTF-8 mapping so non-ASCII bytes survive rather than corrupt.
std::string latin1FieldToUtf8(const uint8_t* s, size_t len) {
    bool hasNonAscii = false;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] >= 0x80) { hasNonAscii = true; break; }
    }
    if (!hasNonAscii) return std::string((const char*)s, len);

    // Try GBK (codepage 936).
    int wlen = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
                                   (const char*)s, (int)len, nullptr, 0);
    if (wlen > 0) {
        std::wstring w(wlen, 0);
        MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
                            (const char*)s, (int)len, &w[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen,
                                       nullptr, 0, nullptr, nullptr);
        std::string out(ulen, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen,
                            &out[0], ulen, nullptr, nullptr);
        return out;
    }
    // Try Shift-JIS (codepage 932).
    wlen = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                               (const char*)s, (int)len, nullptr, 0);
    if (wlen > 0) {
        std::wstring w(wlen, 0);
        MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                            (const char*)s, (int)len, &w[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen,
                                       nullptr, 0, nullptr, nullptr);
        std::string out(ulen, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen,
                            &out[0], ulen, nullptr, nullptr);
        return out;
    }
    // Last resort: strict Latin1 → UTF-8 (U+0080..U+00FF).
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < 0x80) out.push_back((char)s[i]);
        else {
            out.push_back((char)(0xC0 | (s[i] >> 6)));
            out.push_back((char)(0x80 | (s[i] & 0x3F)));
        }
    }
    return out;
}

// Decode an ID3 text field. `enc`: 0=latin1, 1=UTF-16 BOM, 2=UTF-16BE, 3=UTF-8
std::string decodeText(const uint8_t* p, size_t n) {
    if (n == 0) return {};
    uint8_t enc = p[0];
    const uint8_t* s = p + 1;
    size_t len = n - 1;
    if (enc == 3) { // UTF-8
        return std::string((const char*)s, len);
    }
    if (enc == 0) { // ISO-8859-1 marker, but often abused to hold GBK/CP932
        return latin1FieldToUtf8(s, len);
    }
    // UTF-16 (1: BOM, 2: BE without BOM)
    bool be = (enc == 2);
    if (enc == 1 && len >= 2) {
        if (s[0] == 0xFF && s[1] == 0xFE) { be = false; s += 2; len -= 2; }
        else if (s[0] == 0xFE && s[1] == 0xFF) { be = true; s += 2; len -= 2; }
    }
    std::string out;
    out.reserve(len / 2);
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t u = be ? ((uint16_t)s[i] << 8 | s[i+1])
                        : ((uint16_t)s[i+1] << 8 | s[i]);
        if (u == 0) break;
        if (u < 0x80) {
            out.push_back((char)u);
        } else if (u < 0x800) {
            out.push_back((char)(0xC0 | (u >> 6)));
            out.push_back((char)(0x80 | (u & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (u >> 12)));
            out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

// Find a null terminator for a text field (1 byte for enc 0/3, 2 for UTF-16).
size_t textTermLen(uint8_t enc) { return (enc == 1 || enc == 2) ? 2 : 1; }

// ---- ID3v2 (MP3 / AIFF / WAV chunk) --------------------------------------

bool parseId3v2(const uint8_t* data, size_t n, AudioTags& out) {
    if (n < 10 || std::memcmp(data, "ID3", 3) != 0) return false;
    int ver = data[3];                 // 3 = v2.3, 4 = v2.4
    uint8_t flags = data[5];
    uint32_t tagSize = syncsafe(data + 6);
    size_t pos = 10;
    if (pos + tagSize > n) tagSize = (uint32_t)(n - pos);

    // Extended header → skip.
    if (flags & 0x40) {
        if (pos + 4 > n) return false;
        uint32_t extSize = (ver == 4) ? syncsafe(data + pos) : be32(data + pos);
        pos += extSize;
    }

    const size_t tagEnd = std::min((size_t)10 + tagSize, n);
    while (pos + 10 <= tagEnd) {
        const uint8_t* fh = data + pos;
        if (fh[0] == 0) break;
        // v2.2 uses 3-char IDs (TT2/PIC...) — not supported, stop.
        char id[5] = {(char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0};
        uint32_t frameSize = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4);
        if (frameSize == 0 || pos + 10 + frameSize > tagEnd) break;
        const uint8_t* body = fh + 10;
        uint32_t bn = frameSize;

        if (std::memcmp(id, "APIC", 4) == 0 && out.coverData.empty()) {
            // enc(1) | MIME\0 | type(1) | description(enc-dependent)\0 | data
            if (bn >= 2) {
                uint8_t enc = body[0];
                size_t i = 1;
                std::string mime;
                while (i < bn && body[i] != 0) mime.push_back((char)body[i++]);
                ++i; // past MIME null
                if (i < bn) ++i; // picture type byte
                size_t term = textTermLen(enc);
                while (i + term <= bn) {
                    if (term == 1 && body[i] == 0) break;
                    if (term == 2 && body[i] == 0 && body[i+1] == 0) break;
                    ++i;
                }
                i += term;
                if (i < bn) {
                    out.coverData.assign(body + i, body + bn);
                    std::string m = mime;
                    std::transform(m.begin(), m.end(), m.begin(),
                                   [](unsigned char c){ return (char)std::tolower(c); });
                    if (m.find("png") != std::string::npos) out.coverExt = "png";
                    else out.coverExt = "jpg";
                }
            }
        } else if (id[0] == 'T' && id[1] != 'X') {
            std::string txt = decodeText(body, bn);
            // strip trailing nulls/whitespace
            while (!txt.empty() && (txt.back() == 0 || txt.back() == '\r' ||
                                    txt.back() == '\n' || txt.back() == ' '))
                txt.pop_back();
            if (std::memcmp(id, "TIT2", 4) == 0 && out.title.empty())  out.title = txt;
            if (std::memcmp(id, "TPE1", 4) == 0 && out.artist.empty()) out.artist = txt;
            if (std::memcmp(id, "TALB", 4) == 0 && out.album.empty())  out.album = txt;
        }
        pos += 10 + frameSize;
    }
    return true;
}

// ---- FLAC ----------------------------------------------------------------

bool parseFlac(std::ifstream& f, AudioTags& out) {
    char magic[4];
    f.read(magic, 4);
    if (!f || std::memcmp(magic, "fLaC", 4) != 0) return false;

    bool last = false;
    while (!last && f) {
        uint8_t hdr[4];
        f.read((char*)hdr, 4);
        if (!f) break;
        last = (hdr[0] & 0x80) != 0;
        int type = hdr[0] & 0x7f;
        uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        if (type > 126 || len > 16u * 1024 * 1024) break;

        if (type == 4 || type == 6) {
            std::vector<uint8_t> block(len);
            f.read((char*)block.data(), len);
            if (!f) break;

            if (type == 4) {
                // VORBIS_COMMENT: vendor_len(LE) vendor, count(LE), then
                // len(LE)+"KEY=value" pairs.
                size_t p = 0;
                if (p + 4 <= len) {
                    uint32_t vlen = le32(&block[p]); p += 4 + vlen;
                    if (p + 4 <= len) {
                        uint32_t cnt = le32(&block[p]); p += 4;
                        for (uint32_t c = 0; c < cnt && p + 4 <= len; ++c) {
                            uint32_t clen = le32(&block[p]); p += 4;
                            if (p + clen > len) break;
                            std::string kv((const char*)&block[p], clen);
                            p += clen;
                            size_t eq = kv.find('=');
                            if (eq == std::string::npos) continue;
                            std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
                            std::string ku = key;
                            std::transform(ku.begin(), ku.end(), ku.begin(),
                                           [](unsigned char ch){ return (char)std::toupper(ch); });
                            if (ku == "TITLE" && out.title.empty())  out.title = val;
                            if ((ku == "ARTIST" || ku == "ALBUMARTIST") && out.artist.empty())
                                out.artist = val;
                            if (ku == "ALBUM" && out.album.empty())  out.album = val;
                        }
                    }
                }
            } else {
                // PICTURE: type(4) mimeLen(4) mime descLen(4) desc
                //          16 bytes dims dataLen(4) data
                size_t p = 0;
                if (len >= 32) {
                    p += 4; // picture type
                    uint32_t mimeLen = be32(&block[p]); p += 4;
                    std::string mime((const char*)&block[p], mimeLen);
                    p += mimeLen;
                    uint32_t descLen = be32(&block[p]); p += 4 + descLen;
                    p += 16; // width/height/depth/colors
                    if (p + 4 <= len) {
                        uint32_t dataLen = be32(&block[p]); p += 4;
                        if (p + dataLen <= len) {
                            out.coverData.assign(&block[p], &block[p] + dataLen);
                            std::transform(mime.begin(), mime.end(), mime.begin(),
                                           [](unsigned char ch){ return (char)std::tolower(ch); });
                            out.coverExt = (mime.find("png") != std::string::npos) ? "png" : "jpg";
                        }
                    }
                }
            }
        } else {
            f.seekg(len, std::ios::cur);
        }
    }
    return true;
}

// ---- WAV / AIFF: locate an ID3 chunk -------------------------------------

bool parseRiffId3(std::ifstream& f, AudioTags& out) {
    std::error_code ec;
    f.clear();
    f.seekg(0, std::ios::end);
    auto total = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> head(std::min<size_t>(total, 2 * 1024 * 1024));
    f.read((char*)head.data(), head.size());
    size_t got = (size_t)f.gcount();
    for (size_t i = 0; i + 8 < got; ++i) {
        if (std::memcmp(&head[i], "ID3", 3) == 0)
            return parseId3v2(&head[i], got - i, out);
    }
    return false;
}

} // namespace

AudioTags readAudioTags(const std::string& path) {
    AudioTags t;
    std::ifstream f(pathutil::utf8ToWide(path).c_str(), std::ios::binary);
    if (!f) return t;

    // Peek first 4 bytes to dispatch on container.
    char sig[4] = {0};
    f.read(sig, 4);
    f.clear();
    f.seekg(0, std::ios::beg);

    if (std::memcmp(sig, "fLaC", 4) == 0) {
        parseFlac(f, t);
    } else if (std::memcmp(sig, "ID3", 3) == 0) {
        // MP3 with leading ID3v2 tag.
        f.seekg(0, std::ios::end);
        size_t total = (size_t)f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> head(std::min<size_t>(total, 4 * 1024 * 1024));
        f.read((char*)head.data(), head.size());
        parseId3v2(head.data(), f.gcount(), t);
    } else if (std::memcmp(sig, "RIFF", 4) == 0 ||
               std::memcmp(sig, "FORM", 4) == 0) {
        parseRiffId3(f, t);
    }
    t.valid = true;
    return t;
}

} // namespace vj

