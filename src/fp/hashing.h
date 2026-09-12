// Hash generation from spectral peaks — pairs each peak with the next
// FAN_VALUE-1 peaks, hashes (freq1|freq2|tdelta) via SHA1.
#pragma once
#include "peaks.h"
#include <vector>

namespace vj {

std::vector<Fingerprint> generateHashes(const std::vector<Peak>& peaks);

// TEMP STUB (tempo-scaling in progress): hash peaks as if played back at
// `tempoRatio` speed. Currently delegates to generateHashes (ratio ignored).
// Replace with real time-delta scaling once the hashing rewrite lands.
std::vector<Fingerprint> generateHashesScaled(const std::vector<Peak>& peaks,
                                               double tempoRatio);

} // namespace vj
