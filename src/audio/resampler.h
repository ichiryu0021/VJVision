// Streaming linear resampler for mono float signals.
// Input: mono frames at the device mix rate (e.g. 48 kHz).
// Output: mono samples at fp_params::SAMPLE_RATE (44.1 kHz).
// State is kept across blocks so interpolation is continuous.
// (Channel downmixing is done by the caller before feeding samples.)
#pragma once
#include "../fp/fingerprint.h"
#include <vector>

namespace vj {

class MonoResampler {
public:
    MonoResampler() = default;

    void reset(int inRate) {
        inRate_ = inRate > 0 ? inRate : fp_params::SAMPLE_RATE;
        step_ = (double)inRate_ / fp_params::SAMPLE_RATE; // input samples per output sample
        pos_ = 0.0;
        prev_ = 0.f;
    }

    // Process one mono block (already downmixed; zeros when the device
    // reported silence). Appends resampled samples to `out`.
    void process(const float* mono, size_t frames, std::vector<float>& out) {
        if (frames == 0) return;

        // Linear interpolation across blocks.
        // Logical extended buffer: ext[0] = prev_ (last sample of previous
        // block), ext[1..frames] = mono[0..frames-1].
        // pos_ is measured in ext coordinates; after each block it is
        // re-based so 0 means "last sample of this block".
        double pos = pos_;
        const double step = step_;
        while (pos < (double)frames) {
            if (pos >= 0.0) {
                int i = (int)pos;                 // 0..frames-1
                double frac = pos - i;
                float a = (i == 0) ? prev_ : mono[i - 1];
                float b = mono[i];                // i <= frames-1 → valid
                out.push_back((float)(a + (b - a) * frac));
            }
            pos += step;
        }
        pos_ = pos - (double)frames;
        prev_ = mono[frames - 1];
    }

private:
    int inRate_ = 48000;
    double step_ = 48000.0 / 44100.0;
    double pos_ = 0.0;
    float prev_ = 0.f;
};

} // namespace vj
