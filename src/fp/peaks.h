// 2D spectral peak detection — local maxima in a square neighborhood,
// amplitude-filtered. Mirrors dejavu's get_2D_peaks().
#pragma once
#include "stft.h"
#include <vector>

namespace vj {

struct Peak {
    int freqBin;
    int timeFrame;
};

// Extract peaks from a spectrogram.
std::vector<Peak> extractPeaks(const Spectrogram& spec);

} // namespace vj
