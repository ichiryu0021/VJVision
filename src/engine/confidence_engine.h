// Open-source confidence-gate match engine (the classic two-tier gate with
// cumulative offset-coherent vote fallback). Implements IMatchEngine and is
// selected by the factory when no out-of-tree engine is present.
#pragma once
#include "i_match_engine.h"
#include <deque>

namespace vj {

class ConfidenceEngine : public IMatchEngine {
public:
    explicit ConfidenceEngine(MatchParams p) : p_(p) {}

    void reset() override;
    void setParams(const MatchParams& p) override { p_ = p; }
    MatchTick tick(const FpResult& result, double nowSec) override;
    int currentSongId() const override { return currentSongId_; }
    bool transitionActive() const override { return tentativeSongId_ >= 0; }

private:
    void clearCandidate();
    void decayCandidateHold();
    void noteCandidateTick(int votes, double offsetSec);

    static constexpr int kHoldTicks = 5;       // ≈5 s at the 1 s transition cadence
    static constexpr int kEvidenceStreak = 4;  // ticks inside one offset cluster
    static constexpr int kEvidenceVotes = 30;  // cluster aligned-vote threshold
    static constexpr int kEvidenceWindow = 8;  // ticks remembered for clustering
    static constexpr double kOffsetTolSec = 0.35; // cluster radius

    MatchParams p_;
    int currentSongId_ = -1;
    int tentativeSongId_ = -1;
    int tentativeHold_ = 0;
    int breakSongId_ = -1;
    int breakCount_ = 0;
    struct CandTick { double offsetSec; int votes; };
    std::deque<CandTick> candHist_;
    int candStreak_ = 0;
    int candVotes_ = 0;
};

} // namespace vj
