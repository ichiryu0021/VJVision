// Short-Time Fourier Transform — produces a log-power spectrogram.
// Mirrors dejavu's mlab.specgram(NFFT=4096, window=Hann, noverlap=hop).
#pragma once
#include "fingerprint.h"
#include <vector>

namespace vj {

// 2D spectrogram: rows = frequency bins (FFT_WINDOW/2+1), cols = time frames.
// Values are 10*log10(power) in dB, with -inf clamped to a large negative number.
struct Spectrogram {
    int numFreqBins;
    int numFrames;
    std::vector<float> data;  // row-major: data[f * numFrames + t]

    float get(int f, int t) const { return data[f * numFrames + t]; }
    void set(int f, int t, float v) { data[f * numFrames + t] = v; }
};

// Compute the spectrogram for a mono int16 signal.
Spectrogram computeSpectrogram(const int16_t* samples, size_t numSamples, int sampleRate);

} // namespace vj
