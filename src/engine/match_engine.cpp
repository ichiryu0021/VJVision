#include "match_engine.h"
#include <algorithm>
#include <unordered_map>

namespace vj {

void MatchEngine::reset() {
    currentSongId_ = -1;
    pendingSongId_ = -1;
    pendingHits_ = 0;
    tentativeSongId_ = -1;
    breakSongId_ = -1;
    breakCount_ = 0;
    history_.clear();
    inMix_ = false;
    lastReconfirmLog_ = -100.0;
}

MatchTick MatchEngine::tick(const FpResult& r, double nowSec) {
    MatchTick out;
    out.songId = r.songId;
    out.confidence = r.inputConfidence;
    out.offsetSec = r.offsetSec;

    // --- No candidate (silence / below the 0.05 align gate) ---
    if (r.songId < 0 || !r.matched) {
        pendingSongId_ = -1;
        pendingHits_ = 0;
        out.event = MatchEvent::NoMatch;
        return out;
    }

    const float conf = r.inputConfidence;
    const int songId = r.songId;

    // --- Tier 1: noise floor ---
    if (conf < p_.noiseFloor) {
        pendingSongId_ = -1;
        pendingHits_ = 0;
        out.event = MatchEvent::Noise;
        return out;
    }

    const float acceptThreshold = (currentSongId_ < 0)
        ? p_.firstTrackAccept : p_.switchAccept;

    // --- Tier 2: tentative zone ---
    if (conf < acceptThreshold) {
        // First track ever: never show a low-confidence pulsing preview —
        // wait silently for a >= firstTrackAccept hit.
        if (currentSongId_ < 0) {
            out.event = MatchEvent::None;
            return out;
        }

        // During a mix, hold the confirmed display. The outgoing track
        // re-winning ends the mix; anything else keeps pulsing.
        if (inMix_) {
            if (songId == currentSongId_) {
                inMix_ = false;
            }
            out.event = MatchEvent::MixHold;
            return out;
        }

        // A quiet-passage dip on the confirmed track must not downgrade
        // the display to pulsing.
        if (songId == currentSongId_) {
            out.event = MatchEvent::None;
            return out;
        }

        // Tentative preview lock (multi-version songs would otherwise
        // bounce the preview every tick).
        if (tentativeSongId_ < 0) {
            tentativeSongId_ = songId;
            breakSongId_ = -1;
            breakCount_ = 0;
        } else if (songId == tentativeSongId_) {
            breakSongId_ = -1;
            breakCount_ = 0;
        } else {
            if (songId == breakSongId_) {
                ++breakCount_;
            } else {
                breakSongId_ = songId;
                breakCount_ = 1;
            }
            if (breakCount_ >= p_.tentativeBreak) {
                // Genuine switch while still in the tentative band.
                tentativeSongId_ = songId;
                breakSongId_ = -1;
                breakCount_ = 0;
            } else {
                out.event = MatchEvent::None; // lock held
                return out;
            }
        }
        out.event = MatchEvent::Tentative;
        return out;
    }

    // --- Tier 3: confirmed zone (conf >= acceptThreshold) ---
    bool fastConfirm = (songId == tentativeSongId_ && tentativeSongId_ >= 0);
    tentativeSongId_ = -1;
    breakSongId_ = -1;
    breakCount_ = 0;

    // Song-id history for mix detection.
    history_.push_back(songId);
    if ((int)history_.size() > p_.historySize)
        history_.erase(history_.begin());

    // Ongoing verification of the current track.
    if (songId == currentSongId_) {
        pendingSongId_ = -1;
        pendingHits_ = 0;
        inMix_ = false;
        out.event = MatchEvent::None;
        return out;
    }

    // --- Different track: detect a bouncing DJ mix ---
    if (currentSongId_ >= 0 && !inMix_) {
        std::unordered_map<int, int> counts;
        for (int id : history_) ++counts[id];
        int distinct = (int)counts.size();
        int topCount = 0;
        for (const auto& [id, c] : counts) topCount = (std::max)(topCount, c);
        if (distinct >= 2 && topCount <= p_.historySize - 1) {
            inMix_ = true;
            out.event = MatchEvent::MixHold;
            // fall through to mix pending logic below
        }
    }

    if (inMix_ && !fastConfirm) {
        if (songId == pendingSongId_) {
            ++pendingHits_;
        } else {
            pendingSongId_ = songId;
            pendingHits_ = 1;
        }
        if (pendingHits_ < 2) {
            out.event = MatchEvent::MixHold;
            return out;
        }
        // New song won 2 in a row → mix settling, fall through to confirm.
        inMix_ = false;
    }

    int confirmNeeded = fastConfirm ? 1
                       : (currentSongId_ < 0 ? 1 : p_.confirmFrames);
    if (fastConfirm) inMix_ = false;

    if (songId == pendingSongId_) {
        ++pendingHits_;
    } else {
        pendingSongId_ = songId;
        pendingHits_ = 1;
    }

    if (pendingHits_ < confirmNeeded) {
        out.event = MatchEvent::None; // pending (x/confirmNeeded)
        return out;
    }

    // --- Confirmed switch ---
    currentSongId_ = songId;
    pendingSongId_ = -1;
    pendingHits_ = 0;
    inMix_ = false;
    lastReconfirmLog_ = nowSec;
    out.event = MatchEvent::Confirmed;
    return out;
}

} // namespace vj
