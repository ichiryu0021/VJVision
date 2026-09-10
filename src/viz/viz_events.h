// Visualizer event interface.
//
// The recognition/capture side knows nothing about the renderer: it pushes
// Track / Spectrum / Status events through VizSink. Two implementations
// exist today:
//   * QtVizSink   — in-process Qt Quick window (M3)
//   * IpcVizSink  — broadcasts newline-delimited JSON over a Windows named
//                   pipe (\\.\pipe\VJVision_viz) so an external renderer
//                   (e.g. a future Unity player) can subscribe with zero
//                   changes to the C++ core.
// A MulticastSink fans events out to any number of sinks.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace vj {

constexpr int VIZ_SPECTRUM_BINS = 64;

struct TrackEvent {
    bool valid = false;          // false = back to standby
    std::string title;
    std::string artist;
    std::string album;
    std::string coverPath;       // empty → placeholder until M5 TagLib
    float confidence = 0.f;
    bool tentative = false;      // pulsing preview (unconfirmed)
    uint32_t dominantColor = 0;  // 0xAARRGGBB, 0 = not sampled yet
};

enum class VizStatus {
    Standby,     // nothing playing / no track
    Listening,   // capture running, no match yet
    Matching,    // recognition tick in progress
    Mixing,      // DJ cross-fade detected
};

class VizSink {
public:
    virtual ~VizSink() = default;
    virtual void onTrack(const TrackEvent& track) {}
    // bins: VIZ_SPECTRUM_BINS values 0..1; peak: overall level 0..1
    virtual void onSpectrum(const float* bins, int count, float peak) {}
    virtual void onStatus(VizStatus status) {}
};

// Fan-out to multiple sinks (Qt window + IPC broadcaster + ...).
class MulticastSink : public VizSink {
public:
    void add(std::shared_ptr<VizSink> sink) { sinks_.push_back(std::move(sink)); }
    void onTrack(const TrackEvent& t) override {
        for (auto& s : sinks_) s->onTrack(t);
    }
    void onSpectrum(const float* b, int n, float p) override {
        for (auto& s : sinks_) s->onSpectrum(b, n, p);
    }
    void onStatus(VizStatus st) override {
        for (auto& s : sinks_) s->onStatus(st);
    }
private:
    std::vector<std::shared_ptr<VizSink>> sinks_;
};

} // namespace vj
