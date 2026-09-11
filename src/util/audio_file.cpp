#include "audio_file.h"
#include "wav.h"
#include "path_util.h"
#include "../fp/fingerprint.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4244 4267 4456 4505 4334 4100 4189 4702 4245)
#endif
#define DR_FLAC_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_mp3.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vj {

namespace {

// Linear resample of a mono signal to fp_params::SAMPLE_RATE.
std::vector<int16_t> resampleMono(const int16_t* in, size_t n, int srIn, int srOut) {
    if (n == 0) return {};
    double ratio = (double)srOut / srIn;
    size_t outN = (size_t)(n * ratio) + 1;
    std::vector<int16_t> out(outN);
    for (size_t i = 0; i < outN; ++i) {
        double pos = (double)i / ratio;
        size_t i0 = (size_t)std::floor(pos);
        double frac = pos - i0;
        int32_t a = i0 < n ? in[i0] : in[n - 1];
        int32_t b = i0 + 1 < n ? in[i0 + 1] : a;
        out[i] = (int16_t)std::lround(a + (b - a) * frac);
    }
    return out;
}

LoadedAudio fromInterleavedS16(const int16_t* data, size_t frames,
                               int channels, int sampleRate) {
    LoadedAudio a;
    std::vector<int16_t> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        int32_t acc = 0;
        for (int c = 0; c < channels; ++c) acc += data[f * channels + c];
        mono[f] = (int16_t)(acc / channels);
    }
    a.sampleRate = fp_params::SAMPLE_RATE;
    a.monoSamples = (sampleRate != fp_params::SAMPLE_RATE)
        ? resampleMono(mono.data(), mono.size(), sampleRate, fp_params::SAMPLE_RATE)
        : std::move(mono);
    return a;
}

// --- dr_flac via stdio callbacks (wide-path safe) ----------------------

size_t flacRead(void* ud, void* buf, size_t n) {
    return fread(buf, 1, n, (FILE*)ud);
}
drflac_bool32 flacSeek(void* ud, int offset, drflac_seek_origin origin) {
    int whence = SEEK_SET;
    if (origin == DRFLAC_SEEK_CUR) whence = SEEK_CUR;
    else if (origin == DRFLAC_SEEK_END) whence = SEEK_END;
    return fseek((FILE*)ud, offset, whence) == 0
               ? DRFLAC_TRUE : DRFLAC_FALSE;
}
drflac_bool32 flacTell(void* ud, drflac_int64* pos) {
    long p = ftell((FILE*)ud);
    if (p < 0) return DRFLAC_FALSE;
    *pos = p;
    return DRFLAC_TRUE;
}

// Skip one ID3v2 tag at the start of f. Leaves the file positioned at the
// first byte after the ID3 block (the "fLaC" signature). Returns the number
// of bytes skipped, or 0 if no ID3 header was present.
static long skipId3v2(FILE* f) {
    unsigned char hdr[10] = {0};
    if (fread(hdr, 1, 10, f) != 10) { fseek(f, 0, SEEK_SET); return 0; }
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') { fseek(f, 0, SEEK_SET); return 0; }
    // ID3v2 header layout (10 bytes):
    //   [0..2] "ID3"  [3] ver  [4] rev  [5] flags  [6..9] size (synchsafe)
    long size = 0;
    for (int i = 6; i < 10; ++i) size = (size << 7) | (hdr[i] & 0x7F);
    // Flag bit 4 (0x10) means an extended header follows — another 10 bytes.
    if (hdr[5] & 0x10) size += 10;
    long total = 10 + size;
    fseek(f, total, SEEK_SET);
    return total;
}

LoadedAudio loadFlac(const std::string& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, pathutil::utf8ToWide(path).c_str(), L"rb") != 0 || !f)
        throw std::runtime_error("dr_flac: cannot open " + path);

    // Some FLAC files carry an ID3v2 metadata tag before the "fLaC" signature.
    // dr_flac's native ID3-skipper can mis-handle certain configurations,
    // and subsequent FILE* seeks may rewind past our skip point. To avoid
    // both problems, we copy the FLAC stream (after any ID3 prefix) into a
    // contiguous in-memory buffer and let dr_flac parse from there.
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    long skip = skipId3v2(f);
    long flacSize = fileSize - skip;
    if (flacSize <= 0) { fclose(f); throw std::runtime_error("dr_flac: empty after ID3 skip: " + path); }

    std::vector<uint8_t> buf(flacSize);
    size_t bytesRead = fread(buf.data(), 1, flacSize, f);
    fclose(f);  // close before drflac_open_memory — it doesn't need the FILE*
    if (bytesRead != (size_t)flacSize) {
        throw std::runtime_error("dr_flac: short read on " + path);
    }

    drflac* flac = drflac_open_memory(buf.data(), flacSize, nullptr);
    if (!flac) throw std::runtime_error("dr_flac: failed to open " + path);

    int channels = flac->channels;
    int rate = flac->sampleRate;
    std::vector<int16_t> pcm;
    const drflac_uint64 kChunk = 4096;
    int16_t chunk[kChunk * 8];
    drflac_uint64 got = 0;
    while ((got = drflac_read_pcm_frames_s16(flac, kChunk, chunk)) > 0)
        pcm.insert(pcm.end(), chunk, chunk + got * channels);

    drflac_close(flac);
    if (pcm.empty()) throw std::runtime_error("dr_flac: no audio in " + path);
    return fromInterleavedS16(pcm.data(), pcm.size() / channels, channels, rate);
}

LoadedAudio loadMp3(const std::string& path) {
    drmp3 mp3;
    // Wide-path variant avoids ANSI fopen mangling non-ASCII paths.
    if (!drmp3_init_file_w(&mp3, pathutil::utf8ToWide(path).c_str(), nullptr))
        throw std::runtime_error("dr_mp3: failed to open " + path);
    std::vector<int16_t> pcm;
    const size_t kChunk = 4096;
    int16_t chunk[kChunk * 2];
    size_t framesGot = 0;
    while ((framesGot = drmp3_read_pcm_frames_s16(
                &mp3, kChunk, chunk)) > 0) {
        pcm.insert(pcm.end(), chunk, chunk + framesGot * mp3.channels);
    }
    int channels = mp3.channels;
    int rate = mp3.sampleRate;
    drmp3_uninit(&mp3);
    if (pcm.empty()) throw std::runtime_error("dr_mp3: no audio in " + path);
    return fromInterleavedS16(pcm.data(), pcm.size() / channels, channels, rate);
}

} // namespace

LoadedAudio loadAudioFile(const std::string& path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".wav") {
        WavData w = readWav(path);
        LoadedAudio a;
        a.sampleRate = fp_params::SAMPLE_RATE;
        if (w.sampleRate != fp_params::SAMPLE_RATE) {
            a.monoSamples = resampleMono(w.monoSamples.data(), w.monoSamples.size(),
                                         w.sampleRate, fp_params::SAMPLE_RATE);
        } else {
            a.monoSamples = std::move(w.monoSamples);
        }
        return a;
    }
    if (ext == ".flac") return loadFlac(path);
    if (ext == ".mp3")  return loadMp3(path);
    throw std::runtime_error("unsupported format: " + ext);
}

} // namespace vj
