#include "peaks.h"
#include <algorithm>

namespace vj {

std::vector<Peak> extractPeaks(const Spectrogram& spec) {
    const int N = fp_params::PEAK_NEIGHBORHOOD; // 20 → 41x41 window
    const int ampMin = fp_params::AMP_MIN;       // 15 dB
    const int F = spec.numFreqBins;
    const int T = spec.numFrames;

    std::vector<Peak> peaks;
    peaks.reserve(F * T / 100);

    for (int f = N; f < F - N; ++f) {
        for (int t = N; t < T - N; ++t) {
            float val = spec.get(f, t);
            // Skip background (no power) cells.
            if (val <= -199.0f) continue;

            // Check if val is the strict maximum in the 41x41 neighborhood.
            bool isMax = true;
            for (int df = -N; df <= N && isMax; ++df) {
                for (int dt = -N; dt <= N; ++dt) {
                    if (df == 0 && dt == 0) continue;
                    if (spec.get(f + df, t + dt) >= val) {
                        isMax = false;
                        break;
                    }
                }
            }
            if (isMax && val > ampMin) {
                peaks.push_back({f, t});
            }
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
