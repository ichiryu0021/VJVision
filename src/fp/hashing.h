// Hash generation from spectral peaks — pairs each peak with the next
// FAN_VALUE-1 peaks, hashes (freq1|freq2|tdelta) via SHA1.
#pragma once
#include "peaks.h"
#include <vector>

namespace vj {

std::vector<Fingerprint> generateHashes(const std::vector<Peak>& peaks);

} // namespace vj
