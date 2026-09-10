// Minimal WAV/PCM reader — supports 16-bit and 32-bit float PCM, mono or stereo.
// Returns int16 mono samples (averaged down to mono).
#pragma once
#include "path_util.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace vj {

struct WavData {
    int sampleRate;
    int channels;
    std::vector<int16_t> monoSamples;  // averaged to mono, int16
};

inline uint32_t read_u32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t read_u16_le(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
// 24-bit little-endian signed -> int32 (sign-extended)
inline int32_t read_i24_le(const uint8_t* p) {
    int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
    if (v & 0x800000) v |= ~0xFFFFFF;  // sign extend
    return v;
}

inline WavData readWav(const std::string& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, pathutil::utf8ToWide(path).c_str(), L"rb") != 0 || !f)
        throw std::runtime_error("Cannot open WAV: " + path);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(fsize);
    if (fread(buf.data(), 1, fsize, f) != (size_t)fsize) { fclose(f); throw std::runtime_error("WAV read failed"); }
    fclose(f);

    if (fsize < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAV file: " + path);

    // Parse chunks
    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    const uint8_t* dataPtr = nullptr;
    size_t pos = 12;
    while (pos + 8 <= (size_t)fsize) {
        uint32_t chunkId = read_u32_le(buf.data() + pos);
        uint32_t chunkSize = read_u32_le(buf.data() + pos + 4);
        pos += 8;
        if (chunkId == 0x20746D66 /* "fmt " */) {
            audioFmt = read_u16_le(buf.data() + pos);
            channels = read_u16_le(buf.data() + pos + 2);
            sampleRate = read_u32_le(buf.data() + pos + 4);
            bitsPerSample = read_u16_le(buf.data() + pos + 14);
        } else if (chunkId == 0x61746164 /* "data" */) {
            dataPtr = buf.data() + pos;
            dataSize = chunkSize;
            break;
        }
        pos += chunkSize + (chunkSize & 1);
    }

    if (!dataPtr || channels == 0 || sampleRate == 0)
        throw std::runtime_error("Invalid WAV header: " + path);

    WavData result;
    result.sampleRate = (int)sampleRate;
    result.channels = channels;

    size_t bytesPerSample = bitsPerSample / 8;
    size_t totalFrames = dataSize / (bytesPerSample * channels);
    result.monoSamples.reserve(totalFrames);

    for (size_t i = 0; i < totalFrames; ++i) {
        double sum = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            const uint8_t* sp = dataPtr + (i * channels + ch) * bytesPerSample;
            double v;
            if (bitsPerSample == 16) {
                int16_t s = (int16_t)read_u16_le(sp);
                v = s / 32768.0;
            } else if (bitsPerSample == 24) {
                int32_t s = read_i24_le(sp);
                v = s / 8388608.0;
            } else if (bitsPerSample == 32) {
                uint32_t u = read_u32_le(sp);
                float f;
                std::memcpy(&f, &u, 4);
                v = f;
            } else {
                throw std::runtime_error("Unsupported WAV bit depth: " + std::to_string(bitsPerSample));
            }
            sum += v;
        }
        double mono = sum / channels;
        mono = std::max(-1.0, std::min(1.0, mono));
        result.monoSamples.push_back((int16_t)(mono * 32767.0));
    }
    return result;
}

} // namespace vj
