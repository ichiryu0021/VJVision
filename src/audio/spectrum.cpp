#include "spectrum.h"
#include "../../third_party/pffft/pffft.h"

#include <algorithm>
#include <cmath>

namespace vj {

static constexpr double PI = 3.14159265358979323846;

static inline double hzToMel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
static inline double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

SpectrumAnalyzer::SpectrumAnalyzer(int sampleRate) : sr_(sampleRate) {
    // Mel-spaced band edges → FFT bin indices.
    const double minHz = 30.0;
    const double maxHz = (double)sr_ / 2.0;
    const double minMel = hzToMel(minHz);
    const double maxMel = hzToMel(maxHz);
    const int nBins = VIZ_SPECTRUM_BINS;
    binStart_.resize(nBins);
    binEnd_.resize(nBins);
    for (int b = 0; b < nBins; ++b) {
        double f0 = melToHz(minMel + (maxMel - minMel) * (double)b / nBins);
        double f1 = melToHz(minMel + (maxMel - minMel) * (double)(b + 1) / nBins);
        int i0 = std::max(1, (int)std::floor(f0 * FFT_SIZE / sr_));
        int i1 = std::max(i0 + 1, (int)std::ceil(f1 * FFT_SIZE / sr_));
        i1 = std::min(i1, FFT_SIZE / 2);
        binStart_[b] = i0;
        binEnd_[b] = i1;
    }
}

void SpectrumAnalyzer::reset() { gain_ = 1.f; }

void SpectrumAnalyzer::analyze(const float* samples, size_t n, float* outBins) {
    if ((int)windowedBuf_.size() != FFT_SIZE) windowedBuf_.assign(FFT_SIZE, 0.0);
    else std::fill(windowedBuf_.begin(), windowedBuf_.end(), 0.0);
    size_t take = std::min(n, (size_t)FFT_SIZE);
    // Newest samples at the tail (zero-pad at front when not enough).
    size_t offset = FFT_SIZE - take;
    for (size_t i = 0; i < take; ++i) {
        double w = 0.5 - 0.5 * std::cos(2.0 * PI * (double)(i + offset) / (FFT_SIZE - 1));
        windowedBuf_[i + offset] = (double)samples[i] * w;
    }
    auto spec = vjfft::rfft(windowedBuf_.data(), FFT_SIZE);

    float bandMax = 0.f;
    for (int b = 0; b < VIZ_SPECTRUM_BINS; ++b) {
        double power = 0.0;
        int cnt = 0;
        for (int i = binStart_[b]; i < binEnd_[b]; ++i) {
            power += spec[i].real() * spec[i].real() + spec[i].imag() * spec[i].imag();
            ++cnt;
        }
        power /= std::max(1, cnt);
        // rfft is unnormalised: |X| of a full-scale sine peaks at A*N/2,
        // so scale by 2/N to recover spectral amplitude A, then read dBFS.
        // (Using raw power here mapped even the idle noise floor to >1,
        //  which pinned every bar at the top.)
        float mag = (float)(std::sqrt(power) * 2.0 / FFT_SIZE);
        float db = 20.f * std::log10(std::max(mag, 1e-7f));
        float v = (db + 65.f) / 55.f;      // -65 dBFS→0, -10 dBFS→1
        v = std::clamp(v, 0.f, 1.f);
        outBins[b] = v;
        if (v > bandMax) bandMax = v;
    }

    // Slow AGC: drive the loudest band toward ~0.85 with fast attack /
    // slow release so quiet passages stay visible without sticking.
    float target = std::min(4.0f, 0.85f / std::max(bandMax, 0.10f));
    if (target > gain_) gain_ += (target - gain_) * 0.2f;
    else                gain_ += (target - gain_) * 0.02f;
    for (int b = 0; b < VIZ_SPECTRUM_BINS; ++b)
        outBins[b] = std::clamp(outBins[b] * gain_, 0.f, 1.f);
}

} // namespace vj
