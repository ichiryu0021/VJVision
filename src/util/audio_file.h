// Load any supported audio file as mono int16 PCM at 44.1 kHz.
// WAV uses the in-house parser; FLAC/MP3 use the vendored single-header
// dr_flac / dr_mp3 decoders. Non-44.1k sources are linearly resampled.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vj {

struct LoadedAudio {
    std::vector<int16_t> monoSamples;
    int sampleRate = 0;
};

// Throws std::runtime_error on decode failure.
LoadedAudio loadAudioFile(const std::string& path);

} // namespace vj
