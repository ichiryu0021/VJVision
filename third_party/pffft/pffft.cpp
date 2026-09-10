#include "pffft.h"
#include <cmath>

namespace vjfft {

void fft(std::vector<std::complex<double>>& data, bool forward) {
    int n = (int)data.size();
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    double sign = forward ? -1.0 : 1.0;
    for (int len = 2; len <= n; len <<= 1) {
        double ang = sign * 2.0 * 3.14159265358979323846 / len;
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int j = 0; j < len / 2; ++j) {
                auto u = data[i + j];
                auto v = data[i + j + len / 2] * w;
                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<std::complex<double>> rfft(const double* input, int n) {
    std::vector<std::complex<double>> data(n);
    for (int i = 0; i < n; ++i) data[i] = std::complex<double>(input[i], 0.0);
    fft(data, true);
    // Return only the first N/2+1 bins (non-negative frequencies)
    std::vector<std::complex<double>> out(n / 2 + 1);
    for (int i = 0; i <= n / 2; ++i) out[i] = data[i];
    return out;
}

} // namespace vjfft
