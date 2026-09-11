#include "align.h"
#include <unordered_map>

namespace vj {

namespace {
// Combine song_id and time-delta into a single 64-bit key for the histogram.
inline uint64_t makeKey(int songId, int delta) {
    return ((uint64_t)(uint32_t)songId << 32) | (uint64_t)(uint32_t)(delta + 1000000);
}
} // namespace

FpResult alignMatches(const std::vector<Fingerprint>& query,
                      const std::vector<HashHit>& hits,
                      int queryHashCount) {
    FpResult result;
    result.queryHashes = queryHashCount;

    if (hits.empty() || queryHashCount == 0) return result;

    // Histogram: (song_id, delta) -> vote count, where delta = dbOffset - queryOffset.
    std::unordered_map<uint64_t, int> hist;
    hist.reserve(hits.size());

    for (const auto& h : hits) {
        int delta = h.dbOffset - h.queryOffset;
        uint64_t key = makeKey(h.songId, delta);
        hist[key]++;
    }

    int bestSong = -1;
    int bestVotes = 0;
    int bestDelta = 0;
    for (const auto& [key, count] : hist) {
        if (count > bestVotes) {
            bestVotes = count;
            bestSong = (int)(key >> 32);
            bestDelta = (int)(key & 0xFFFFFFFF) - 1000000;
        }
    }

    if (bestSong < 0) return result;

    result.songId = bestSong;
    result.alignedVotes = bestVotes;
    result.inputConfidence = (float)bestVotes / queryHashCount;
    result.offsetSec = (double)bestDelta * fp_params::HOP_SIZE / fp_params::SAMPLE_RATE;
    result.matched = result.inputConfidence >= fp_params::MIN_CONFIDENCE;
    return result;
}

} // namespace vj
