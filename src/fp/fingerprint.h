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
    int alignedVotes = 0;   // votes in the best (song, delta) histogram bin
    double sliceSec = 0.0;  // length of the tail sub-window that produced it
    bool localAgc = false;  // produced by the short tail pass with its own AGC
};

// Dejavu-tuned parameters.
// 2.3: denser grid (50% overlap → hop 2048), lower dB floor, wider fan-out.
// The old sparse set (~4-6k hashes/track) was extremely sensitive to the
// STFT grid phase: a live ring window starts on an arbitrary sample, and a
// sub-hop shift moved spectral peaks enough that most 80-bit hashes never
// reproduced. Denser fingerprints plus a 4-phase query sweep (viz worker)
// recover essentially all live windows. Any change here invalidates stored
// hashes → bump FP_SCHEMA_VERSION so FpDb wipes and forces a reindex.
namespace fp_params {
    constexpr int SAMPLE_RATE = 44100;
    constexpr int FFT_WINDOW = 4096;
    constexpr double OVERLAP_RATIO = 0.5;
    constexpr int HOP_SIZE = static_cast<int>(FFT_WINDOW * (1.0 - OVERLAP_RATIO)); // 2048
    constexpr int FAN_VALUE = 8;
    constexpr int AMP_MIN = 12;            // dB
    constexpr int PEAK_NEIGHBORHOOD = 20;  // 41x41 square window
    constexpr int HASH_REDUCTION = 20;     // hex chars from SHA1
    constexpr int MIN_HASH_TIME_DELTA = 0;
    constexpr int MAX_HASH_TIME_DELTA = 200;
    constexpr float MIN_CONFIDENCE = 0.05f;

    // Stored-fingerprint compatibility version (SQLite PRAGMA user_version).
    // v1 = hop3072/amp15/fan3; v2 = hop2048/amp12/fan8.
    constexpr int FP_SCHEMA_VERSION = 2;
}

} // namespace vj
