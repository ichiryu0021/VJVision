#include "confidence_engine.h"
#include <algorithm>
#include <cmath>

namespace vj {

void ConfidenceEngine::clearCandidate() {
    tentativeSongId_ = -1;
    tentativeHold_ = 0;
    breakSongId_ = -1;
    breakCount_ = 0;
    candHist_.clear();
    candStreak_ = 0;
    candVotes_ = 0;
}

void ConfidenceEngine::decayCandidateHold() {
    if (tentativeSongId_ < 0) return;
    if (--tentativeHold_ <= 0) clearCandidate();
}

void ConfidenceEngine::noteCandidateTick(int votes, double offsetSec) {
    tentativeHold_ = kHoldTicks;
    candHist_.push_back({offsetSec, (std::max)(0, votes)});
    while ((int)candHist_.size() > kEvidenceWindow)
        candHist_.pop_front();

    // Vote-weighted clustering over recent ticks: genuine alignment sits at
    // one (slowly BPM-drifting) offset every tick; coincidence hits scatter
    // across random offsets and never stack. O(window^2), window = 8.
    int bestVotes = 0;
    int bestTicks = 0;
    for (const auto& a : candHist_) {
        int v = 0;
        int n = 0;
        for (const auto& b : candHist_) {
            if (std::fabs(b.offsetSec - a.offsetSec) <= kOffsetTolSec) {
                v += b.votes;
                ++n;
            }
        }
        if (v > bestVotes) {
            bestVotes = v;
            bestTicks = n;
        }
    }
    candStreak_ = bestTicks;
    candVotes_ = bestVotes;
}

void ConfidenceEngine::reset() {
    currentSongId_ = -1;
    clearCandidate();
}

MatchTick ConfidenceEngine::tick(const FpResult& r, double /*nowSec*/) {
    MatchTick out;
    out.songId = r.songId;
    out.confidence = r.inputConfidence;
    out.offsetSec = r.offsetSec;

    if (r.songId < 0 || !r.matched) {
        decayCandidateHold();
        out.event = MatchEvent::NoMatch;
        return out;
    }

    const float conf = r.inputConfidence;
    const int songId = r.songId;

    // Tier 1: noise floor.
    if (conf < p_.noiseFloor) {
        decayCandidateHold();
        out.event = MatchEvent::Noise;
        return out;
    }

    const float acceptThreshold = (currentSongId_ < 0)
        ? p_.firstTrackAccept : p_.switchAccept;

    // Tier 2: below the accept gate → accumulate offset-coherent votes.
    if (conf < acceptThreshold) {
        if (songId == currentSongId_) {
            decayCandidateHold();
            out.event = MatchEvent::None;
            return out;
        }

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
                clearCandidate();
                tentativeSongId_ = songId;
                tentativeHold_ = kHoldTicks;
            } else {
                out.event = MatchEvent::None;
                decayCandidateHold();
                return out;
            }
        }

        noteCandidateTick(r.alignedVotes, r.offsetSec);
        if (candStreak_ >= kEvidenceStreak && candVotes_ >= kEvidenceVotes) {
            const int totalVotes = candVotes_;
            currentSongId_ = songId;
            clearCandidate();
            out.event = MatchEvent::Confirmed;
            out.evidenceConfirmed = true;
            out.evidenceVotes = totalVotes;
            return out;
        }

        out.streakTicks = candStreak_;
        out.streakVotes = candVotes_;
        out.event = (currentSongId_ < 0) ? MatchEvent::Tentative
                                         : MatchEvent::MixHold;
        return out;
    }

    // Tier 3: conf >= accept gate.
    if (songId == currentSongId_) {
        decayCandidateHold();
        out.event = MatchEvent::None;
        return out;
    }

    // One decisive high-confidence alignment switches immediately.
    if (conf >= p_.forceSwitch) {
        currentSongId_ = songId;
        clearCandidate();
        out.event = MatchEvent::Confirmed;
        out.forceConfirmed = true;
        return out;
    }

    currentSongId_ = songId;
    clearCandidate();
    out.event = MatchEvent::Confirmed;
    return out;
}

// Factory (open-source build).
std::unique_ptr<IMatchEngine> createMatchEngine(const MatchParams& params) {
    return std::make_unique<ConfidenceEngine>(params);
}

} // namespace vj

