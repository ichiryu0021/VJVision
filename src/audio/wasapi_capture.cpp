// WASAPI capture implementation (Windows).
#include "wasapi_capture.h"
#include "resampler.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vj {

namespace {

struct EndpointInfo {
    std::wstring id;
    std::wstring name;
};

std::vector<EndpointInfo> enumEndpoints(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<EndpointInfo> out;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col)
        return out;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;
        EndpointInfo ei;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            ei.id = id;
            CoTaskMemFree(id);
        }
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                ei.name = pv.pwszVal;
            }
            PropVariantClear(&pv);
            store->Release();
        }
        dev->Release();
        out.push_back(std::move(ei));
    }
    col->Release();
    return out;
}

bool isFloat32Format(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32)
        return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (wfx->wBitsPerSample == 32 &&
            IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return true;
    }
    return false;
}

} // namespace

std::vector<DeviceInfo> WasapiCapture::listDevices() {
    std::vector<DeviceInfo> result;
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        auto renders = enumEndpoints(en, eRender);
        auto captures = enumEndpoints(en, eCapture);
        int idx = 0;
        for (const auto& e : renders)
            result.push_back({idx++, e.id, e.name, true, 0, 0});
        for (const auto& e : captures)
            result.push_back({idx++, e.id, e.name, false, 0, 0});
        en->Release();
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
    return result;
}

WasapiCapture::~WasapiCapture() { close(); }

bool WasapiCapture::open(int index) {
    wantedIndex_ = index;
    return true;
}

bool WasapiCapture::start(RingBuffer* ring) {
    if (running_.exchange(true)) return false;
    ring_ = ring;
    lost_.store(false);
    thread_ = std::thread(&WasapiCapture::threadFunc, this);
    return true;
}

void WasapiCapture::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void WasapiCapture::close() { stop(); }

void WasapiCapture::threadFunc() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) {
        fprintf(stderr, "[wasapi] CoInitializeEx failed: 0x%08lx — capture disabled\n",
                (unsigned long)coHr);
        lost_.store(true);
        return;
    }
    while (running_.load()) {
        if (runOnce()) break;  // true = stop requested
        // false = device missing/lost: runOnce already slept; retry.
    }
    CoUninitialize();
}

// Returns true when the thread should exit (stop requested),
// false when the device was lost / unavailable and a retry is needed.
bool WasapiCapture::runOnce() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    bool isLoopback = true;
    IMMDevice* dev = nullptr;
    HRESULT hr;
    if (wantedIndex_ < 0) {
        isLoopback = true;
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    } else {
        auto renders = enumEndpoints(en, eRender);
        if (wantedIndex_ < (int)renders.size()) {
            isLoopback = true;
            hr = en->GetDevice(renders[wantedIndex_].id.c_str(), &dev);
        } else {
            isLoopback = false;
            auto captures = enumEndpoints(en, eCapture);
            int ci = wantedIndex_ - (int)renders.size();
            if (ci >= 0 && ci < (int)captures.size())
                hr = en->GetDevice(captures[ci].id.c_str(), &dev);
            else
                hr = E_NOTFOUND;
        }
    }
    en->Release();

    if (FAILED(hr) || !dev) {
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    IAudioClient* client = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    dev->Release();
    if (FAILED(hr) || !client) {
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    WAVEFORMATEX* wfx = nullptr;
    hr = client->GetMixFormat(&wfx);
    if (FAILED(hr) || !wfx) {
        client->Release();
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    int sr = (int)wfx->nSamplesPerSec;
    int ch = (int)wfx->nChannels;
    bool floatFmt = isFloat32Format(wfx);

    DWORD streamFlags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    const REFERENCE_TIME bufferDuration = 5'000'000; // 0.5 s
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                            bufferDuration, 0, wfx, nullptr);
    CoTaskMemFree(wfx);
    if (FAILED(hr)) {
        fprintf(stderr, "[wasapi] Initialize failed: 0x%08lx\n", (unsigned long)hr);
        client->Release();
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    if (!floatFmt) {
        // Shared-mode mix format is virtually always float32; refuse rather
        // than mis-handle integer formats.
        fprintf(stderr, "[wasapi] unsupported mix format (need float32)\n");
        client->Release();
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    IAudioCaptureClient* cap = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    if (FAILED(hr) || !cap) {
        client->Release();
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        cap->Release();
        client->Release();
        lost_.store(true);
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }

    fprintf(stderr, "[wasapi] OK: sr=%d ch=%d float32 loopback=%d\n", sr, ch, isLoopback);
    activeSr_.store(sr);
    activeCh_.store(ch);
    lost_.store(false);
    if (ring_) ring_->clear();

    MonoResampler resampler;
    resampler.reset(sr);

    std::vector<float> mono;
    std::vector<float> out;
    out.reserve(8192);
    bool deviceLost = false;
    float blockPeak = 0.f;
    double blockSq = 0.0;
    size_t blockSamples = 0;

    while (running_.load()) {
        Sleep(8);
        // Advance the block timer even if no packets arrive — loopback may
        // produce zero packets when the render endpoint is truly silent.
        // Without this, blockPeak never resets and peak_ stays high forever.
        blockSamples += sr / 125;   // ~8 ms worth, matches Sleep(8)
        if (blockSamples > (size_t)sr / 2) {
            peak_.store(blockPeak);
            rms_.store((float)std::sqrt(blockSq / (double)blockSamples));
            blockPeak = 0.f;
            blockSq = 0.0;
            blockSamples = 0;
        }

        UINT32 packetLen = 0;
        hr = cap->GetNextPacketSize(&packetLen);
        if (FAILED(hr)) {
            deviceLost = (hr == AUDCLNT_E_DEVICE_INVALIDATED);
            break;
        }
        while (packetLen > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                deviceLost = (hr == AUDCLNT_E_DEVICE_INVALIDATED);
                break;
            }
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            mono.resize(frames);
            if (silent || !data) {
                std::fill(mono.begin(), mono.end(), 0.f);
                blockSamples += frames; // silence counts toward the level window
            } else {
                const float* f = reinterpret_cast<const float*>(data);
                float inv = 1.0f / (float)ch;
                for (UINT32 i = 0; i < frames; ++i) {
                    float s = 0.f;
                    for (int c = 0; c < ch; ++c) s += f[i * ch + c];
                    s *= inv;
                    mono[i] = s;
                    float a = s < 0.f ? -s : s;
                    if (a > blockPeak) blockPeak = a;
                    blockSq += (double)s * s;
                }
                blockSamples += frames;
            }
            resampler.process(mono.data(), frames, out);

            hr = cap->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                deviceLost = (hr == AUDCLNT_E_DEVICE_INVALIDATED);
                break;
            }
            hr = cap->GetNextPacketSize(&packetLen);
            if (FAILED(hr)) {
                deviceLost = (hr == AUDCLNT_E_DEVICE_INVALIDATED);
                break;
            }
        }
        if (!out.empty()) {
            if (ring_) ring_->write(out.data(), out.size());
            out.clear();
        }
        // (block reset moved to loop top — always runs, even with no packets)
        if (deviceLost) break;
    }

    client->Stop();
    cap->Release();
    client->Release();

    if (deviceLost) {
        lost_.store(true);
        activeSr_.store(0);
        if (ring_) ring_->clear();
        for (int i = 0; i < 15 && running_.load(); ++i) Sleep(100);
        return false;
    }
    return true; // stop requested → exit thread
}

} // namespace vj
