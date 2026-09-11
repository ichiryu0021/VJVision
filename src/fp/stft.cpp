#include "stft.h"
#include "../../third_party/pffft/pffft.h"
#include <cmath>
#include <cstring>

namespace vj {

namespace {
std::vector<float> hannWindow(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * i / (n - 1)));
    return w;
}
} // namespace

Spectrogram computeSpectrogram(const int16_t* samples, size_t numSamples, int sampleRate) {
    const int N = fp_params::FFT_WINDOW;
    const int hop = fp_params::HOP_SIZE;
    const int numFrames = numSamples >= (size_t)N
        ? (int)((numSamples - N) / hop) + 1
        : 0;
    const int numFreq = N / 2 + 1;

    Spectrogram spec;
    spec.numFreqBins = numFreq;
    spec.numFrames = numFrames;
    spec.data.assign((size_t)numFreq * numFrames, -200.0f);

    if (numFrames == 0) return spec;

    auto hann = hannWindow(N);
    std::vector<double> frame(N);

    for (int t = 0; t < numFrames; ++t) {
        double sum = 0.0;
        for (int i = 0; i < N; ++i) {
            double v = (double)samples[t * hop + i] / 32768.0;
            frame[i] = v;
            sum += v;
        }
        double mean = sum / N;
        for (int i = 0; i < N; ++i)
            frame[i] = (frame[i] - mean) * hann[i];

        auto bins = vjfft::rfft(frame.data(), N);
        for (int f = 0; f < numFreq; ++f) {
            double re = bins[f].real();
            double im = bins[f].imag();
            double power = re * re + im * im;
            float db = power > 1e-20 ? 10.0f * (float)std::log10(power) : -200.0f;
            spec.set(f, t, db);
        }
    }
    return spec;
}

} // namespace vj
