#include "peaks.h"
#include <algorithm>
#include <vector>

namespace vj {

// 41x41 strict-maximum peak extraction.
//
// The naïve implementation compared every interior cell against all 1681
// neighbors (O(41^2 * F * T)); with the denser hop-2048 grid and a 4-phase
// query sweep that became hundreds of millions of comparisons per tick.
// Maximum filtering is separable (max over a square = max of the two 1D
// maxima), and each 1D pass is O(n) with a monotonic deque — exact same
// maxima as brute force, ties included. Only cells equal to the window max
// (potential plateau ties) fall back to an explicit uniqueness scan; the
// strict "no neighbor >= val" rule is therefore bit-for-bit identical to the
// original behavior.
std::vector<Peak> extractPeaks(const Spectrogram& spec) {
    const int N = fp_params::PEAK_NEIGHBORHOOD; // 20 → 41x41 window
    const int ampMin = fp_params::AMP_MIN;
    const int F = spec.numFreqBins;
    const int T = spec.numFrames;

    std::vector<Peak> peaks;
    if (F <= 2 * N || T <= 2 * N) return peaks;
    peaks.reserve(F * T / 100);

    const int total = F * T;
    std::vector<float> tMax(total);   // max along time (radius N)
    std::vector<float> sqMax(total);  // max along frequency of tMax
    std::vector<int> dq((std::max)(F, T));

    // Pass 1: per-frequency-row sliding maximum over time (contiguous data).
    for (int f = 0; f < F; ++f) {
        const float* row = spec.data.data() + (size_t)f * T;
        float* out = tMax.data() + (size_t)f * T;
        int head = 0, tail = 0;
        for (int i = 0; i < T; ++i) {
            while (tail > head && row[dq[tail - 1]] <= row[i]) --tail;
            dq[tail++] = i;
            if (dq[head] < i - 2 * N) ++head;          // window [i-2N, i]
            if (i >= 2 * N) out[i - N] = row[dq[head]];
        }
    }

    // Pass 2: per-time-column sliding maximum over frequency (stride T).
    for (int t = 0; t < T; ++t) {
        int head = 0, tail = 0;
        for (int i = 0; i < F; ++i) {
            const float v = tMax[(size_t)i * T + t];
            while (tail > head && tMax[(size_t)dq[tail - 1] * T + t] <= v)
                --tail;
            dq[tail++] = i;
            if (dq[head] < i - 2 * N) ++head;
            if (i >= 2 * N)
                sqMax[(size_t)(i - N) * T + t] = tMax[(size_t)dq[head] * T + t];
        }
    }

    for (int f = N; f < F - N; ++f) {
        for (int t = N; t < T - N; ++t) {
            const float val = spec.get(f, t);
            if (val <= -199.0f || val <= ampMin) continue;
            if (sqMax[(size_t)f * T + t] != val) continue;
            // Window maximum reached — reject plateau ties (a neighbor with
            // an equal value): the strict-maximum rule from the original.
            bool unique = true;
            for (int df = -N; df <= N && unique; ++df) {
                for (int dt = -N; dt <= N; ++dt) {
                    if (df == 0 && dt == 0) continue;
                    if (spec.get(f + df, t + dt) >= val) {
                        unique = false;
                        break;
                    }
                }
            }
            if (unique) peaks.push_back({f, t});
        }
    }

    // Sort peaks by time frame (then frequency) — dejavu sorts by time.
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) {
        if (a.timeFrame != b.timeFrame) return a.timeFrame < b.timeFrame;
        return a.freqBin < b.freqBin;
    });

    return peaks;
}

} // namespace vj
