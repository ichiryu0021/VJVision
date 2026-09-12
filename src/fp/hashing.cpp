#include "hashing.h"
#include "../util/sha1.h"
#include <cstdio>

namespace vj {

std::vector<Fingerprint> generateHashes(const std::vector<Peak>& peaks) {
    std::vector<Fingerprint> hashes;
    hashes.reserve(peaks.size() * (fp_params::FAN_VALUE - 1));

    const int fan = fp_params::FAN_VALUE;
    const int minDelta = fp_params::MIN_HASH_TIME_DELTA;
    const int maxDelta = fp_params::MAX_HASH_TIME_DELTA;
    const int dtQ = fp_params::DT_QUANT;
    const int freqQ = fp_params::FREQ_QUANT;

    for (size_t i = 0; i < peaks.size(); ++i) {
        for (int j = 1; j < fan; ++j) {
            size_t k = i + j;
            if (k >= peaks.size()) break;
            int t1 = peaks[i].timeFrame;
            int t2 = peaks[k].timeFrame;
            int tDelta = t2 - t1;
            if (tDelta < minDelta || tDelta > maxDelta) continue;
            // Quantize dt and freq bins so hashes tolerate small tempo
            // changes (keylock-on beatmatch) and spectral artifacts.
            tDelta /= dtQ;
            int f1 = peaks[i].freqBin / freqQ;
            int f2 = peaks[k].freqBin / freqQ;

            char buf[64];
            int n = std::snprintf(buf, sizeof(buf), "%d|%d|%d", f1, f2, tDelta);
            std::string h = sha1_hex_prefix(buf, (size_t)n, fp_params::HASH_REDUCTION);
            hashes.push_back({std::move(h), t1});
        }
    }
    return hashes;
}

} // namespace vj
