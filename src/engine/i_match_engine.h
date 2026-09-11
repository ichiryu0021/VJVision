// Match-engine abstraction — the decision layer on top of fingerprint
// alignment. alignMatches() produces raw evidence (song id, aligned votes,
// db/query offset, confidence); an IMatchEngine implementation decides
// when to lock the first track, when to switch during a DJ blend, and when
// to hold the on-screen track through FX/scratch passages.
//
// Two interchangeable implementations are supported:
//   * ConfidenceEngine (open source, src/engine/confidence_engine.cpp) —
//     the classic two-tier confidence gate with cumulative-vote fallback.
//   * An optional out-of-tree engine may be supplied by a local,
//     non-published source tree. CMake links it automatically when present;
//     without it the project builds fully from the open sources.
//
// MatchParams is a superset: the open engine uses the threshold fields,
// while alternative engines may ignore them. Keep new fallback parameters in
// this struct so future engines do not need new plumbing.
#pragma once
#include "../fp/fingerprint.h"
#include <memory>

namespace vj {

enum class MatchEvent {
    None,          // nothing to report (silent hold / cooldown)
    NoMatch,       // no candidate at all (silence / below the align gate)
    Noise,         // candidate below the engine's admit threshold
    Tentative,     // first-track candidate accumulating (display unchanged)
    MixHold,       // switch candidate accumulating, holding previous track
    Confirmed,     // track locked / switched (songId set)
};

struct MatchTick {
    MatchEvent event = MatchEvent::None;
    int songId = -1;
    double confidence = 0.0;
    double offsetSec = 0.0;

    // Open confidence-engine evidence markers.
    bool evidenceConfirmed = false; // locked via cross-tick offset cluster
    bool forceConfirmed = false;    // single decisive high-conf switch
    int evidenceVotes = 0;          // votes in the cluster behind it
    int streakTicks = 0;            // ticks supporting the best cluster
    int streakVotes = 0;            // aligned votes inside that cluster

    // Optional engine vote/slot state (zero/-1 on engines that do not use it).
    int curVotes = 0;
    int curSongId = -1;
};

struct MatchParams {
    // Open confidence-engine thresholds (unused by alternative engines).
    float noiseFloor = 0.10f;
    float firstTrackAccept = 0.25f;
    float switchAccept = 0.30f;
    float forceSwitch = 0.50f;
    int   tentativeBreak = 3;
    // Reserve for future fallback/safety parameters of either engine.
};

class IMatchEngine {
public:
    virtual ~IMatchEngine() = default;

    // Reset all state (new visualization session / standby restart).
    virtual void reset() = 0;

    // Live parameter update from the control panel; applied on next tick.
    virtual void setParams(const MatchParams& p) = 0;

    // Feed one alignment result. `nowSec` is a monotonic clock in seconds.
    virtual MatchTick tick(const FpResult& result, double nowSec) = 0;

    // Hard-locked track currently on screen (-1 while the slot is empty).
    virtual int currentSongId() const = 0;

    // True while a transition is underway: the worker shortens the query
    // window and enables the local-AGC tail pass in this mode.
    virtual bool transitionActive() const = 0;
};

// Factory — implemented once by whichever engine is linked in.
std::unique_ptr<IMatchEngine> createMatchEngine(const MatchParams& params);

} // namespace vj
