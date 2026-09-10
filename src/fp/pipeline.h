// End-to-end fingerprint generation for a mono int16 signal:
// STFT -> peak extraction -> fan-out SHA1 hashes.
// Shared by the CLI index/query path and the parallel indexer.
#pragma once
#include "fingerprint.h"
#include "stft.h"
#include "peaks.h"
#include "hashing.h"
#include <cstdio>
#include <cstdint>
#include <vector>

namespace vj {

inline std::vector<Fingerprint> fingerprintSignal(const int16_t* samples, size_t n, int sr) {
    if (sr != fp_params::SAMPLE_RATE) {
        fprintf(stderr, "WARNING: sample rate %d != %d — resample before fingerprinting\n",
                sr, fp_params::SAMPLE_RATE);
    }
    Spectrogram spec = computeSpectrogram(samples, n, sr);
    std::vector<Peak> peaks = extractPeaks(spec);
    return generateHashes(peaks);
}

} // namespace vj
