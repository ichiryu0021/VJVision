// VJVision "Music Battery" match engine (音乐电池引擎 / ChargeBarEngine) —
// a leaky-bucket decision state machine for DJ setlists.
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
#pragma once
#include "engine/i_match_engine.h"

namespace vj {

class ChargeBarEngine : public IMatchEngine {
public:
    ChargeBarEngine() = default;
    explicit ChargeBarEngine(const MatchParams& p) : params_(p) {}

    void reset() override;
    void setParams(const MatchParams& p) override { params_ = p; }
    MatchTick tick(const FpResult& r, double nowSec) override;

    int  currentSongId() const override { return currentSongId_; }
    bool transitionActive() const override {
        return currentSongId_ < 0 || curBar_ < kBarCap;
    }

private:
    // ---- 电量条结构常量 ----
    static constexpr int    kBarCap       = 10;   // 满格票数
    // 漏电按 0.5s/拍校准：1 格/拍（满格零进票约 5s 漏到 0，但漏到 0 不退场）。
    static constexpr int    kBarLeak      = 1;    // 漏电时代每拍漏掉的格数
    static constexpr int    kAdmitVotes   = 4;    // 入场票：低于此的命中不进任何条
    static constexpr int    kBreakHits    = 4;    // 别的歌连续 4 拍（≈2s）抢候选锁
    static constexpr double kHoldSec      = 6.0;  // 候选条未充满时无进票的存活秒数
    static constexpr int    kReanchorHits = 3;    // 候选歌连续 3 拍偏移漂移 → 重锚
    static constexpr double kOffsetTolSec = 0.35; // 候选条只收锁定偏移 ±0.35s 内的票

    // ---- 空槽首曲跨时间复核门（音乐电池引擎兜底）----
    // 未锁定时识别每 0.5s 一拍、切片约 4.5s，相邻切片重叠约 90%：一拍高票
    // 即可充满，两拍重叠窗口的巧合误匹配等于同一份证据算两次。故空槽期
    // 候选满格后，还需跨时间复核：≥kFirstTrackHits 个充电拍，且两次充电
    // 之间不限制时间——候选在空槽期不受 kHoldSec 存活期约束（见
    // ageCandidate），真歌偏移稳定，早晚凑齐第二拍即上位；巧合误匹配的
    // 偏移在拍间乱跳，过不了 ±0.35s 锚定，永远凑不出第二拍。仅作用于
    // 空槽首曲，进槽后的切歌保持原速度，不加此门。
    static constexpr int    kFirstTrackHits   = 2;   // 空槽首曲最少充电拍数

    // 引擎每拍输出：只报告状态，切不切歌完全由引擎内部状态机决定。
    // 电量条（leaky bucket）模型：
    //   "正在播放"是一个槽位。空槽时（待机首曲 / 无信号回待机后）所有歌都
    //   不漏电，候选歌只充电，第一首充满 kBarCap 格的歌进槽。
    //   进槽后进入漏电时代：槽内歌与候选歌每拍先漏 kBarLeak 格。
    //   槽内歌电量归零也不退场——DJ 表演中 FX/搓碟会使票数暂时归零但歌还
    //   在放；只要还有信号它就留在槽位（画面保留），直到下一首歌充满换槽。
    //   退回待机只由"无信号"决定（worker 静音超时 → reset()）。
    MatchParams params_;

    int  currentSongId_ = -1;
    int  curBar_ = 0;                 // 槽内歌电量（进票不校验偏移）
    bool slotFilled_ = false;         // 漏电时代：已有歌进槽（reset 前持续）

    int    tentativeSongId_ = -1;     // 候选（挑战者 / 空槽期的第一首歌）
    int    candBar_ = 0;
    double candAnchor_ = -1.0;        // 锁定的候选播放偏移
    double candHoldUntil_ = 0.0;
    int    candAgeTicks_ = 0;
    int    breakCount_ = 0;           // 别的歌连续抢中拍数
    int    nonCohStreak_ = 0;         // 候选歌偏移漂移连续拍数
    int    candHits_ = 0;             // 候选累计充电拍数（首曲复核门用）
    double candFirstChargeSec_ = -1.0; // 候选首次充电时刻（首曲复核门用）

    void clearCandidate();
    void leakBars();
    void ageCandidate(double nowSec);
    // 候选进票：累计充电拍数/首次充电时刻（首曲复核门）并刷新存活期。
    void chargeCandidate(int votes, double nowSec);
};

} // namespace vj
