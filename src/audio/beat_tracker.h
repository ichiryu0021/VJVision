// Real-time energy-based beat tracker operating on PRE-AGC Mel bins.
//
// Why this lives in C++ rather than QML: the bins exposed to the renderer
// pass through the SpectrumAnalyzer's slow AGC (fast attack / ~1.6 s
// release). After every kick the gain recovery ramp lifts ALL bands for
// dozens of frames, which any frame-difference onset detector reads as a
// stream of fake onsets. This tracker consumes the raw dB-mapped bands
// before that gain is applied, so the signal is uncontaminated.
//
// Method (energy only, no tempo/BPM estimation):
//   On a spectrogram a kick is a sharp vertical stripe at the low end:
//   a 1-2 frame rise concentrated in the low Mel bands (30..~330 Hz)
//   accompanied by a simultaneous rise across many bands. A sustained
//   bassline/wobble modulation moves only a few low bands over several
//   frames; snare/hi-hat transients sit in the mid/high bands. The onset
//   detection function is therefore
//       odf = lowBandPositiveFlux * coincidenceFactor
//   and an onset fires on EITHER of two paths (both with a refractory
//   guard):
//     A — odf above the fixed gate with a frame-to-frame rising edge
//     B — frame-to-frame odf jump above kStrongJump regardless of the
//         absolute level (catches light kicks inside dense passages)
//   beat_ is a fast-attack / slow-release envelope around each onset.
#pragma once
#include <vector>

namespace vj {

class BeatTracker {
public:
    // rawBins: VIZ_SPECTRUM_BINS dB-mapped 0..1 values BEFORE the visual
    // AGC. All-zero buffers are fine during silence (pass silent=true).
    // dt: seconds since the previous call (~0.033 at 30 fps).
    void process(const float* rawBins, int count, float dt, bool silent);

    void reset();

    float beat() const { return beat_; }   // 0..1 decaying pulse envelope
    bool  onset() const { return onset_; } // true on the frame a beat fires

private:
    static constexpr int   kLowEnd         = 6;     // flux bins 0..5 ≈ 30-308 Hz
                                                    // (user-tuned cap ~330 Hz;
                                                    // narrows out vocal chest
                                                    // resonance above 300 Hz)
    static constexpr float kBeatTauMs      = 280.f; // envelope release
    // 110 ms allows rapid kick rolls / beat triplets (128 BPM triplet =
    // 156 ms; 16th-note at 150 BPM = 100 ms).
    static constexpr float kRefractoryMs   = 110.f;
    // Fixed gate, user-tuned on real material (adaptive mean/median gates
    // both failed: they float up in dense passages and down in quiet ones).
    static constexpr float kGate           = 0.25f;
    // Per-band dead band: positive bin differences below this are overlap
    // window / clamp jitter, not signal — they must NOT enter lowFlux or
    // they build a fake floor between kicks.
    static constexpr float kLowBinEps      = 0.015f;
    static constexpr float kBandRiseEps    = 0.004f;
    // Path A: above the gate + sharp frame-to-frame rising edge.
    static constexpr float kRiseProminence = 0.01f;
    // Path B: transient jump regardless of absolute level — catches quiet
    // kicks whose odf stays below the gate (vocal/synth churn < 0.05/frame).
    static constexpr float kStrongJump     = 0.08f;

    std::vector<float> prevBins_;
    float beat_    = 0.f;
    bool  onset_   = false;
    float timeSinceOnsetMs_ = 10000.f;
    float prevOdf_ = 0.f;
};

} // namespace vj
