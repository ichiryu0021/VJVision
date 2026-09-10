// Streaming spectrum analyzer for visualization.
//
// The viz worker polls the recognition ring buffer ~30 times per second and
// hands over the most recent FFT_SIZE samples. Each window is Hann-windowed,
// rFFT'd and folded into VIZ_SPECTRUM_BINS Mel-spaced bands (30 Hz..16 kHz),
// dB-converted and normalized to 0..1 with a slow automatic gain.
// Peak-hold/smoothing is left to the renderer (QML side).
#pragma once
#include "../viz/viz_events.h"
#include <cstdint>
#include <vector>

namespace vj {

class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(int sampleRate = 44100);

    // Analyze one window (expects FFT_SIZE samples; shorter inputs are
    // zero-padded at the front). Writes VIZ_SPECTRUM_BINS values 0..1.
    void analyze(const float* samples, size_t n, float* outBins);

    static constexpr int FFT_SIZE = 2048;

    void reset();

private:
    int sr_;
    std::vector<int> binStart_;   // FFT bin range per Mel band
    std::vector<int> binEnd_;
    float gain_ = 1.f;           // slow AGC for visual normalization
};

} // namespace vj
