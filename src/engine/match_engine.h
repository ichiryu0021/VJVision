// Recognition state machine — port of matcher.py's two-tier confidence gate.
//
// States per recognition tick:
//   idle       → no confirmed track yet
//   tentative  → low-confidence candidate shown as a pulsing preview
//   confirmed  → a track is locked on screen
//
// Rules (defaults from config.py):
//   * confidence < 0.13 (noise floor)        → reject as noise, hold display
//   * 0.13 <= confidence < accept threshold  → tentative pulse preview
//   * confidence >= accept threshold         → confirmation counter
//       - first track:   accept at 0.25, 1 hit
//       - track switch:  accept at 0.30, 2 consecutive hits
//   * tentative previews lock to one song; a *different* song must win 3
//     consecutive tentative hits to break the lock (multi-version songs
//     otherwise bounce the preview every tick)
//   * DJ mix: recent high-confidence hits bouncing between >=2 songs →
//     hold the current display until the new song wins 2 in a row
#pragma once
#include "../fp/fingerprint.h"
#include <string>
#include <vector>

namespace vj {

enum class MatchEvent {
    None,          // nothing to report (silent hold / cooldown)
    NoMatch,       // no candidate at all (silence / below 0.05 align gate)
    Noise,         // candidate below the noise floor
    Tentative,     // low-confidence pulsing preview (songId set)
    MixHold,       // DJ mix detected, holding the confirmed track
    Confirmed,     // track locked / switched (songId set)
};

struct MatchTick {
    MatchEvent event = MatchEvent::None;
    int songId = -1;
    float confidence = 0.f;
    double offsetSec = 0.0;
};

struct MatchParams {
    float noiseFloor = 0.13f;        // tentative_min_confidence
    float firstTrackAccept = 0.25f;  // first_track_min_confidence
    float switchAccept = 0.30f;      // switch_min_confidence
    int   confirmFrames = 1;         // match_confirmations (consecutive hits)
    int   tentativeBreak = 3;        // consecutive different-song tentative hits to release lock
    int   historySize = 4;           // mix detection window
};

class MatchEngine {
public:
    MatchEngine() = default;
    explicit MatchEngine(MatchParams p) : p_(p) {}

    void reset();

    // Live parameter update (control panel). Applied on the next tick;
    // thresholds take effect immediately, confirmation counters / state
    // machine progress are preserved.
    void setParams(const MatchParams& p) { p_ = p; }
    const MatchParams& params() const { return p_; }

    // Feed one recognition result (from alignMatches). `nowSec` is a
    // monotonic clock in seconds, used for the re-confirm cooldown.
    MatchTick tick(const FpResult& result, double nowSec);

    int currentSongId() const { return currentSongId_; }
    bool inMix() const { return inMix_; }

private:
    MatchParams p_;
    int currentSongId_ = -1;    // hard-confirmed track on screen
    int pendingSongId_ = -1;    // consecutive-hit counter for confirmation
    int pendingHits_ = 0;
    int tentativeSongId_ = -1;  // locked pulsing preview
    int breakSongId_ = -1;      // different-song streak trying to break the lock
    int breakCount_ = 0;
    std::vector<int> history_;  // recent high-confidence song ids
    bool inMix_ = false;
    double lastReconfirmLog_ = -100.0; // throttle ReConfirmed-style reports
};

} // namespace vj
