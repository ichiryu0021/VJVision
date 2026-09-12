// Match-engine abstraction — the decision layer on top of fingerprint
// alignment. alignMatches() produces raw evidence (song id, aligned votes,
// db/query offset, confidence); an IMatchEngine implementation decides
// when to lock the first track, when to switch during a DJ blend, and when
// to hold the on-screen track through FX/scratch passages.
//
// VJVision ships one implementation: the "Music Battery" ChargeBarEngine
// (src/engine/charge_engine.cpp), MIT licensed with required source
// attribution — see the copyright header in that file.
//
// MatchParams is the parameter struct: the ChargeBar engine currently
// ignores its threshold fields; keep new fallback parameters in this struct
// so future engines do not need new plumbing.
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

    // Evidence markers (used to tag the confirmation in log lines).
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
    // Threshold fields kept for interface stability (unused by the
    // ChargeBar engine; may drive alternative engines in the future).
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
