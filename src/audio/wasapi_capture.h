// WASAPI audio capture: device enumeration + loopback (system mix) / input
// capture. Audio is downmixed to mono, linearly resampled to 44.1 kHz and
// written into a RingBuffer. Device loss (unplug / default-device change)
// is detected and the capture auto-reconnects without crashing.
#pragma once
#include "ring_buffer.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace vj {

struct DeviceInfo {
    int index = -1;
    std::wstring id;        // WASAPI endpoint id
    std::wstring name;      // friendly name
    bool isLoopback = false;// true = render endpoint captured via loopback
    int channels = 0;
    int sampleRate = 0;
};

class WasapiCapture {
public:
    // Enumerate active endpoints. Render endpoints come first (loopback),
    // then capture endpoints. Thread-safe; initializes COM internally.
    static std::vector<DeviceInfo> listDevices();

    WasapiCapture() = default;
    ~WasapiCapture();

    // Select device: -1 = default render endpoint (system loopback).
    // Does not block; actual open happens on the capture thread so that
    // missing devices can be retried.
    // NOTE: the enumeration index is volatile (reorders on plug/unplug);
    // GUI/persisted selections should prefer openEndpoint() with the
    // stable WASAPI endpoint id from DeviceInfo::id.
    bool open(int index);

    // Select device by stable WASAPI endpoint id (DeviceInfo::id).
    // Empty id = default render endpoint (system loopback). The id is
    // resolved on the capture thread; if it has vanished the capture
    // falls back to the default render loopback and keeps retrying.
    bool openEndpoint(const std::wstring& endpointId);

    bool start(RingBuffer* ring);
    void stop();
    void close();

    // Levels from the most recent audio block (0..1).
    float peakLevel() const { return peak_.load(); }
    float rmsLevel() const { return rms_.load(); }

    // True while the endpoint is unavailable and the thread is retrying.
    bool deviceLost() const { return lost_.load(); }

    int activeSampleRate() const { return activeSr_.load(); }
    int activeChannels() const { return activeCh_.load(); }

private:
    void threadFunc();
    // One open/run/teardown cycle. Returns false if the device was lost
    // (caller should retry); true means stop was requested.
    bool runOnce();

    int wantedIndex_ = -1;
    std::wstring wantedId_;   // stable endpoint id (used when selectById_)
    bool selectById_ = false; // true = openEndpoint(), false = open(index)
    RingBuffer* ring_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<float> peak_{0.f};
    std::atomic<float> rms_{0.f};
    std::atomic<bool> lost_{false};
    std::atomic<int> activeSr_{0};
    std::atomic<int> activeCh_{0};
};

} // namespace vj
