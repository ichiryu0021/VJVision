#include "beat_tracker.h"

#include <algorithm>
#include <cmath>

namespace vj {

void BeatTracker::reset() {
    std::fill(prevBins_.begin(), prevBins_.end(), 0.f);
    beat_ = 0.f;
    onset_ = false;
    timeSinceOnsetMs_ = 10000.f;
    prevOdf_ = 0.f;
}

void BeatTracker::process(const float* rawBins, int count, float dt, bool silent) {
    onset_ = false;

    if (!rawBins || count < 24) {
        beat_ = std::max(0.f, beat_ - 0.02f);
        return;
    }

    if ((int)prevBins_.size() != count) {
        prevBins_.assign(count, 0.f);
        beat_ = 0.f;
        timeSinceOnsetMs_ = 10000.f;
        prevOdf_ = 0.f;
    }

    // --- Energy onset detection function on PRE-AGC log bins ---
    // Positive frame difference over the KICK-ONLY sub range (30..308 Hz,
    // user-tuned to stay below ~330 Hz and reject vocal chest resonance).
    // Per-band dead band kLowBinEps is essential: with 2048-sample windows
    // at a 33 ms hop (72% overlap) and dB clamping at -65 dB, summing EVERY
    // positive micro-difference builds a persistent jitter floor (~0.2)
    // between kicks. Only differences above the dead band count.
    float lowFlux = 0.f;
    int riseCount = 0;
    const int lowEnd = std::min(kLowEnd, count);
    for (int b = 0; b < count; ++b) {
        float d = rawBins[b] - prevBins_[b];
        prevBins_[b] = rawBins[b];
        if (d > kLowBinEps && b < lowEnd) lowFlux += d;
        if (d > kBandRiseEps) ++riseCount;
    }
    float coincide = std::min(1.f, riseCount / 12.f);
    float odf = silent ? 0.f : lowFlux * (0.85f + 0.15f * coincide);

    // Visual pulse envelope: instant attack, ~280 ms release.
    beat_ *= std::exp(-dt * 1000.f / kBeatTauMs);
    timeSinceOnsetMs_ += dt * 1000.f;

    float jump = odf - prevOdf_;
    // Path A: above fixed gate + rising edge (ordinary kick).
    // Path B: strong transient jump regardless of level (light kick).
    bool pathA = odf > kGate && jump > kRiseProminence;
    bool pathB = jump > kStrongJump;
    if (!silent && (pathA || pathB) && timeSinceOnsetMs_ > kRefractoryMs) {
        onset_ = true;
        beat_ = 1.f;
        timeSinceOnsetMs_ = 0.f;
    }
    prevOdf_ = odf;
}

} // namespace vj
