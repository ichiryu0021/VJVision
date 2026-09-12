// VJVision "Music Battery" match engine (音乐电池引擎 / ChargeBarEngine).
//
// Copyright (c) 2026 ichiryu (https://github.com/ichiryu0021/VJVision)
// SPDX-License-Identifier: MIT
//
// Source attribution / 来源声明:
//   This file originates from the VJVision project's "Music Battery"
//   (ChargeBar) engine. If you redistribute or derive from this file in
//   source or binary form, you MUST retain this copyright header and
//   attribute the work as:
//     "Music Battery (ChargeBar) engine by ichiryu, from VJVision
//      (https://github.com/ichiryu0021/VJVision), MIT License."
//   See the repository LICENSE file for the full MIT license text.
#include "charge_engine.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace vj {

void ChargeBarEngine::reset() {
    currentSongId_ = -1;
    curBar_ = 0;
    slotFilled_ = false;
    clearCandidate();
    breakCount_ = 0;
}

void ChargeBarEngine::clearCandidate() {
    tentativeSongId_ = -1;
    candBar_ = 0;
    candAnchor_ = -1.0;
    candHoldUntil_ = 0.0;
    candAgeTicks_ = 0;
    nonCohStreak_ = 0;
    candHits_ = 0;
    candFirstChargeSec_ = -1.0;
}

void ChargeBarEngine::leakBars() {
    // 空槽期（首曲/无信号回待机后）不漏电；进槽后每拍漏两条。
    if (!slotFilled_) return;
    curBar_ = std::max(0, curBar_ - kBarLeak);
    candBar_ = std::max(0, candBar_ - kBarLeak);
}

void ChargeBarEngine::ageCandidate(double nowSec) {
    // 空槽首曲：两次充电确认不限制时间——空槽期候选不因 kHoldSec
    // 存活期过期，真歌的第二拍无论多久后出现都算数；巧合误匹配的
    // ±0.35s 锚定票在拍间乱跳，不会凑出第二次。进槽后的挑战者仍受
    // 存活期约束（切歌速度不变）。
    if (!slotFilled_) return;
    if (tentativeSongId_ >= 0 && nowSec > candHoldUntil_) {
        clearCandidate();
    }
}

void ChargeBarEngine::chargeCandidate(int votes, double nowSec) {
    if (candFirstChargeSec_ < 0.0) candFirstChargeSec_ = nowSec;
    ++candHits_;
    candBar_ = std::min(kBarCap, candBar_ + votes);
    candHoldUntil_ = nowSec + kHoldSec;
}

MatchTick ChargeBarEngine::tick(const FpResult& r, double nowSec) {
    MatchTick out;
    out.songId = r.songId;
    out.confidence = r.inputConfidence;
    out.offsetSec = r.offsetSec;

    if (tentativeSongId_ >= 0) ++candAgeTicks_;

    // 1) 漏电时代每拍先漏电（再在下方加本拍进票）
    leakBars();

    // 入场票：同 (song,delta) 对齐票达到 kAdmitVotes 即允许充电。
    // 不要求 offsetSec>=0 —— 会话开头零填充/搓碟回针会使归一化偏移暂时
    // 为负，真歌高票曾因此被长期挡在门外（event=Noise 不充电）。负偏移
    // 的噪声误匹配会在拍间乱跳，过不了候选的 ±0.35s 锚定，也凑不满
    // kBarCap（80 位 hash 空间 10 票巧合概率可忽略）。
    const bool admitted = (r.matched && r.songId >= 0 &&
                           r.alignedVotes >= kAdmitVotes);

    // 2) 槽内歌进票：不校验偏移，任何同歌入场票都直接充当前条。
    //    电量可漏到 0，但不退场——有信号就仍在槽位，直到下一首充满换槽。
    if (admitted && r.songId == currentSongId_) {
        curBar_ = std::min(kBarCap, curBar_ + r.alignedVotes);
    }

    const bool slotOpen = (currentSongId_ < 0 || curBar_ < kBarCap);

    // 3) 候选（挑战者 / 空槽期第一首）逻辑
    if (!admitted) {
        ageCandidate(nowSec);
    } else if (r.songId == currentSongId_) {
        breakCount_ = 0;
        ageCandidate(nowSec);
    } else {
        if (r.songId != tentativeSongId_) {
            // 别的歌：连续 kBreakHits 拍抢中 → 候选锁换人（旧条作废）
            ++breakCount_;
            if (breakCount_ >= kBreakHits) {
                tentativeSongId_ = r.songId;
                candBar_ = 0;
                candAnchor_ = -1.0;
                candHoldUntil_ = nowSec + kHoldSec;
                candAgeTicks_ = 0;
                nonCohStreak_ = 0;
                candHits_ = 0;
                candFirstChargeSec_ = -1.0;
            }
        } else {
            breakCount_ = 0;
        }

        if (r.songId == tentativeSongId_) {
            if (candBar_ == 0 || candAnchor_ < 0.0) {
                candAnchor_ = r.offsetSec;
                nonCohStreak_ = 0;
            }
            const bool coherent =
                std::fabs(r.offsetSec - candAnchor_) <= kOffsetTolSec;
            if (coherent) {
                nonCohStreak_ = 0;
                chargeCandidate(r.alignedVotes, nowSec);
            } else {
                // 偏移漂移：连续 kReanchorHits 拍就重锚（不清零已充电量）
                ++nonCohStreak_;
                if (nonCohStreak_ >= kReanchorHits) {
                    candAnchor_ = r.offsetSec;
                    nonCohStreak_ = 0;
                    chargeCandidate(r.alignedVotes, nowSec);
                } else {
                    ageCandidate(nowSec);
                }
            }
        }
    }

    // 4) 换槽：候选充满，且槽位为空 或 槽内歌已不满格。
    //    空槽首曲另需通过跨时间复核门（见头文件 kFirstTrackHits 说明）：
    //    满格后还要凑够 2 个充电拍，两次充电之间不限制时间。满格未过门时
    //    保持 Tentative，继续充电等门开。
    const bool barFull = (tentativeSongId_ >= 0 && candBar_ >= kBarCap);
    bool firstTrackGate = true;
    if (barFull && !slotFilled_) {
        firstTrackGate = (candHits_ >= kFirstTrackHits);
    }
    if (barFull && slotOpen && firstTrackGate) {
        out.event = MatchEvent::Confirmed;
        out.evidenceVotes = kBarCap;
        currentSongId_ = tentativeSongId_;
        curBar_ = kBarCap;
        slotFilled_ = true;
        out.songId = currentSongId_;
        clearCandidate();
        breakCount_ = 0;

        out.streakVotes = kBarCap;
        out.curVotes = kBarCap;
        out.curSongId = currentSongId_;
        return out;
    }

    // 5) 未切换：输出状态标签
    out.curVotes = curBar_;
    out.streakVotes = candBar_;
    out.streakTicks = candAgeTicks_;
    out.curSongId = currentSongId_;
    if (tentativeSongId_ >= 0) {
        out.event = (currentSongId_ < 0) ? MatchEvent::Tentative
                                         : MatchEvent::MixHold;
        out.songId = tentativeSongId_;
    } else if (!admitted) {
        out.event = (r.matched && r.alignedVotes > 0) ? MatchEvent::Noise
                                                      : MatchEvent::NoMatch;
    } else {
        out.event = MatchEvent::None;
    }
    return out;
}

// Factory — the Music Battery (ChargeBar) engine is VJVision's engine.
std::unique_ptr<IMatchEngine> createMatchEngine(const MatchParams& params) {
    return std::make_unique<ChargeBarEngine>(params);
}

} // namespace vj
