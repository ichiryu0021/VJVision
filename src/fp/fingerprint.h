// Common fingerprint types shared across the fp module.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vj {

// A single fingerprint: (hash, offset_in_frames).
// hash is the first 20 hex chars of SHA1("freq1|freq2|tdelta").
struct Fingerprint {
    std::string hash;
    int offset;
};

// A database hit for a query hash.
struct HashHit {
    int songId;
    int dbOffset;
    int queryOffset;  // the offset of the query hash that produced this hit
};

// Result of matching a query against the database.
struct FpResult {
    bool matched = false;
    int songId = -1;
    std::string songName;
    float inputConfidence = 0.f;
    double offsetSec = 0.0;
    int queryHashes = 0;
};

// Dejavu-tuned parameters (from VJVision fingerprint.py _optimize_dejavu_params).
namespace fp_params {
    constexpr int SAMPLE_RATE = 44100;
    constexpr int FFT_WINDOW = 4096;
    constexpr double OVERLAP_RATIO = 0.25;
    constexpr int HOP_SIZE = static_cast<int>(FFT_WINDOW * (1.0 - OVERLAP_RATIO)); // 3072
    constexpr int FAN_VALUE = 3;
    constexpr int AMP_MIN = 15;            // dB
    constexpr int PEAK_NEIGHBORHOOD = 20;  // 41x41 square window
    constexpr int HASH_REDUCTION = 20;     // hex chars from SHA1
    constexpr int MIN_HASH_TIME_DELTA = 0;
    constexpr int MAX_HASH_TIME_DELTA = 200;
    constexpr float MIN_CONFIDENCE = 0.05f;
}

} // namespace vj
