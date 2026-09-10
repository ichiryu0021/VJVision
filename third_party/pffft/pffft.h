// Minimal in-place radix-2 FFT (M0 spike — correctness over speed).
// Replaces pffft for the M0 milestone; switch to pffft/fftw3 later.
#pragma once
#include <vector>
#include <complex>

namespace vjfft {

// In-place radix-2 DIT FFT. `data` must have length that is a power of 2.
// forward=true for forward FFT, false for inverse (unnormalized).
void fft(std::vector<std::complex<double>>& data, bool forward);

// Real FFT: takes N real samples, returns N/2+1 complex bins.
// Bin 0 = DC, bin N/2 = Nyquist, bins 1..N/2-1 are positive frequencies.
std::vector<std::complex<double>> rfft(const double* input, int n);

} // namespace vjfft
