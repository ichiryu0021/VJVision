#include "viz_controller.h"
#include "qt_viz.h"
#include "ipc_pipe.h"

#include "../audio/wasapi_capture.h"
#include "../audio/ring_buffer.h"
#include "../audio/spectrum.h"
#include "../audio/beat_tracker.h"
#include "../fp/fingerprint.h"
#include "../fp/pipeline.h"
#include "../fp/fp_db.h"
#include "../fp/align.h"
#include "../util/tags.h"
#include "../util/path_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace vj {

namespace {
// Minimum evidence for a short tail slice to be allowed to BEAT the full
// window result. 10 aligned votes in the 80-bit hash space is far beyond
// chance-collision probability; the full window stays the baseline so sparse
// tracks that genuinely produce fewer votes are never rejected outright.
constexpr int kMinSliceHashes = 80;
constexpr int kMinAlignedVotes = 10;

// STFT grid phase sweep. DB fingerprints lie on each indexed file's own hop
// grid, but the live ring window opens on an arbitrary sample; a sub-hop
// grid shift drifts spectral peaks and breaks nearly all 80-bit hashes
// (measured: old params 0/19 windows at unlucky phases). Query each window
// on 4 grids offset by 1/4 hop and keep the strongest alignment — worst-case
// distance to the true grid drops from 3/4 hop to 1/8 hop. Every phase
// slice ends at the same "now" sample, so offset normalization is shared.
constexpr int kQueryPhases = 4;

// SQL lookup happens ONCE on the full query; hashes and hits are then
// partitioned in memory by their query offset (frame units) and aligned over
// several tail sub-windows, keeping the strongest. A 12 s (or 6 s) window
// dilutes confidence two ways: during a slow fader blend only the last
// seconds are dominated by the incoming track, and sparse intros contain
// long low-density stretches. A pure tail of such audio aligns far better
// than the whole-window average.
FpResult alignBestTail(const std::vector<Fingerprint>& fps,
                       const std::vector<HashHit>& hits) {
    FpResult best;
    if (fps.empty()) return best;
    int maxOff = 0;
    for (const auto& f : fps) maxOff = (std::max)(maxOff, f.offset);
    const double framesPerSec =
        (double)fp_params::SAMPLE_RATE / fp_params::HOP_SIZE;

    // Tail fractions of the query window; slices shorter than 2 s are too
    // noisy to be meaningful.
    const double tailFracs[] = {1.0, 0.5, 0.35};
    for (double frac : tailFracs) {
        const int cutoff = (int)(maxOff * (1.0 - frac));
        const double sliceSec = (maxOff - cutoff + 1) / framesPerSec;
        if (sliceSec < 2.0) continue;

        std::vector<Fingerprint> slice;
        slice.reserve(fps.size() / 4 + 1);
        for (const auto& f : fps)
            if (f.offset >= cutoff) slice.push_back(f);

        std::vector<HashHit> sliceHits;
        sliceHits.reserve(hits.size() / 4 + 1);
        for (const auto& h : hits)
            if (h.queryOffset >= cutoff) sliceHits.push_back(h);

        FpResult r = alignMatches(slice, sliceHits, (int)slice.size());
        if (!r.matched) continue;
        // Full window is the unguarded baseline; shorter slices must clear
        // both minimum-hash and minimum-aligned-vote gates to override it.
        if (frac < 1.0 && ((int)slice.size() < kMinSliceHashes ||
                           r.alignedVotes < kMinAlignedVotes))
            continue;
        r.sliceSec = sliceSec;
        if (r.inputConfidence > best.inputConfidence) best = r;
    }
    return best;
}
} // namespace

VizController::VizController(QObject* parent) : QObject(parent) {}

VizController::~VizController() {
    stop();
}

float VizController::peak() const {
    return qtSink_ ? qtSink_->peak() : 0.f;
}

void VizController::setMatchParams(const MatchParams& p) {
    std::lock_guard<std::mutex> lk(paramsMtx_);
    params_ = p;
}

MatchParams VizController::matchParams() const {
    std::lock_guard<std::mutex> lk(paramsMtx_);
    return params_;
}

void VizController::setStandbyPath(const QString& path) {
    if (qtSink_) qtSink_->setStandbyPath(path);
}
void VizController::setBgVideoPath(const QString& path) {
    if (qtSink_) qtSink_->setBgVideoPath(path);
}
void VizController::setBgOverlayDepth(float v) {
    if (qtSink_) qtSink_->setBgOverlayDepth(v);
}
void VizController::setBgColor(const QString& hex) {
    if (qtSink_) qtSink_->setBgColor(hex);
}
void VizController::setVizMode(int v) {
    if (qtSink_) qtSink_->setVizMode(v);
}
void VizController::setBgMode(int v) {
    if (qtSink_) qtSink_->setBgMode(v);
}
void VizController::setFxTexture(int v) {
    if (qtSink_) qtSink_->setFxTexture(v);
}
void VizController::setPerformanceMode(int v) {
    if (qtSink_) qtSink_->setPerformanceMode(v);
}
void VizController::setLogoSizeStandby(float v) {
    if (qtSink_) qtSink_->setLogoSizeStandby(v);
}
void VizController::setLogoSizePlaying(float v) {
    if (qtSink_) qtSink_->setLogoSizePlaying(v);
}

bool VizController::start(const std::string& dbPath, const std::wstring& deviceId) {
    if (running_.load()) return false;

    // DB is now optional — empty dbPath → viz runs in standby-only mode,
    // no track matching but still shows spectrum + captures audio.
    if (!dbPath.empty()) {
        FpDb db;
        if (!db.open(dbPath)) {
            emit logMessage(QStringLiteral("WARNING: cannot open DB: %1 — running without track matching")
                                .arg(QString::fromStdString(dbPath)));
            // Continue — no DB = no track matching, but viz still works.
        } else if (db.listSongs().empty()) {
            emit logMessage(QStringLiteral("DB has no indexed songs — no track matching, but viz still starts."));
        }
    } else {
        emit logMessage(QStringLiteral("No DB selected — running in standby-only mode."));
    }

    qtSink_ = std::make_shared<QtVizSink>();
    // Windowed — user drags to any monitor, then presses F to fullscreen there.
    if (!qtSink_->load(-1)) {
        emit logMessage(QStringLiteral("ERROR: failed to load visualizer QML."));
        qtSink_.reset();
        return false;
    }

    // Auto-load standby image. Search order:
    //   <exe_dir>/data/standby.png (portable) → <exe_dir>/standby.png →
    //   <dataDir>/standby.png (installed: ~/Documents/VJVision_data, where the DB lives)
    {
        QDir exeDir(QCoreApplication::applicationDirPath());
        QString standby = exeDir.filePath(QStringLiteral("data/standby.png"));
        if (!QFile::exists(standby))
            standby = exeDir.filePath(QStringLiteral("standby.png"));
        if (!QFile::exists(standby) && !dbPath.empty()) {
            QDir dbDir(QString::fromStdString(fs::path(dbPath).parent_path().string()));
            QString dbStandby = dbDir.filePath(QStringLiteral("standby.png"));
            if (QFile::exists(dbStandby)) standby = dbStandby;
        }
        if (QFile::exists(standby)) {
            qtSink_->setStandbyPath(QStringLiteral("file:///") + QDir::toNativeSeparators(standby).replace('\\', '/'));
            emit logMessage(QStringLiteral("Standby image: %1").arg(standby));
        }
    }

    // Auto-load background video. Same search order as the standby image above.
    {
        QDir exeDir(QCoreApplication::applicationDirPath());
        QString video = exeDir.filePath(QStringLiteral("data/bg_video.mp4"));
        if (!QFile::exists(video))
            video = exeDir.filePath(QStringLiteral("bg_video.mp4"));
        if (!QFile::exists(video) && !dbPath.empty()) {
            QDir dbDir(QString::fromStdString(fs::path(dbPath).parent_path().string()));
            QString dbVideo = dbDir.filePath(QStringLiteral("bg_video.mp4"));
            if (QFile::exists(dbVideo)) video = dbVideo;
        }
        if (QFile::exists(video)) {
            qtSink_->setBgVideoPath(QStringLiteral("file:///") + QDir::toNativeSeparators(video).replace('\\', '/'));
            emit logMessage(QStringLiteral("Background video: %1").arg(video));
        }
    }
    // Escape on the fullscreen window → full teardown, panel stays.
    QObject::connect(qtSink_.get(), &QtVizSink::closeRequested, this,
        [this] { stop(); }, Qt::QueuedConnection);
    ipcSink_ = std::make_shared<IpcVizSink>();
    ipcSink_->start();
    multicast_ = std::make_shared<MulticastSink>();
    multicast_->add(qtSink_);
    multicast_->add(ipcSink_);

    running_.store(true);
    worker_ = std::thread(&VizController::workerFunc, this, dbPath, deviceId);
    const QString deviceLabel = deviceId.empty()
        ? QStringLiteral("default loopback")
        : QStringLiteral("selected endpoint");
    emit logMessage(QStringLiteral("Visualizer started (device=%1, windowed — drag + F for fullscreen)")
                        .arg(deviceLabel));
    emit sessionStarted();
    return true;
}

void VizController::stop() {
    // Fully idempotent: a second call (aboutToQuit, destructor, or a
    // queued Escape racing with window close) must not re-enter shutdown
    // — re-emitting sessionStopped during QApplication teardown re-enters
    // the quit path and overflows the stack.
    const bool idle = !running_.load() && !worker_.joinable() &&
                      !ipcSink_ && !qtSink_ && !multicast_;
    if (idle) return;

    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (ipcSink_) {
        ipcSink_->stop();
        ipcSink_.reset();
    }
    if (qtSink_) {
        qtSink_.reset();   // destroys the QML window (GUI thread)
    }
    multicast_.reset();
    emit sessionStopped();
}

void VizController::workerFunc(std::string dbPath, std::wstring deviceId) {
    auto log = [this](const std::string& s) {
        emit logMessage(QString::fromStdString(s));
    };

    FpDb db;
    const bool hasDb = !dbPath.empty() && db.open(dbPath);
    if (!hasDb && !dbPath.empty()) {
        log("Cannot open DB " + dbPath + " — continuing without track matching.");
    } else if (!hasDb) {
        log("No DB — running without track matching.");
    }

    RingBuffer ring((size_t)fp_params::SAMPLE_RATE * 30);
    WasapiCapture cap;
    cap.openEndpoint(deviceId);
    if (!cap.start(&ring)) {
        log("Failed to start audio capture (device unavailable).");
        return;
    }

    SpectrumAnalyzer analyzer(fp_params::SAMPLE_RATE);
    BeatTracker beatTracker;
    auto engine = createMatchEngine(matchParams());
    std::vector<float> bins(VIZ_SPECTRUM_BINS, 0.f);
    std::vector<float> rawBins(VIZ_SPECTRUM_BINS, 0.f);

    // Query window: long while a track is stable (robust against sparse
    // passages), shortened while a transition is detected (DJ EQ sweep /
    // overlap / slow fader push-up — the incoming track's hashes are diluted
    // by a long window, so shrinking makes it cross the accept gate near the
    // blend tail instead of long after the mix has finished).
    constexpr size_t windowSamplesStable = (size_t)fp_params::SAMPLE_RATE * 12;
    constexpr size_t windowSamplesTransition = (size_t)fp_params::SAMPLE_RATE * 6;
    // Extra prefix room for the deepest 1/4-hop phase offset; every phase
    // slice keeps the full window length and ends at the latest sample.
    constexpr size_t kMaxPhaseOff =
        (size_t)fp_params::HOP_SIZE * (kQueryPhases - 1) / kQueryPhases;
    std::vector<int16_t> i16(windowSamplesStable + kMaxPhaseOff);

    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();
    auto nextTick = clock::now();   // start immediately — don't wait 3s
    auto nextSpectrum = clock::now();
    auto lastBeatTick = clock::now();

    // Track silent→audible edge so we can fire recognition immediately
    // when music comes back (don't wait for the next tick slot).
    bool wasSilent = true;
    // AGC 尾通道在过渡态隔拍执行（0.5s 节拍 → 每 1s 一次），控制 CPU。
    int transitionTickIdx = 0;

    auto* sink = static_cast<VizSink*>(multicast_.get());
    sink->onStatus(VizStatus::Standby);
    VizStatus lastStatus = VizStatus::Standby;
    auto setStatus = [&](VizStatus s) {
        if (s != lastStatus) { lastStatus = s; sink->onStatus(s); }
    };

    // Silence auto-timeout: after N seconds of silence, clear the track
    // so the UI fades back to standby.
    const auto silenceTimeout = std::chrono::seconds(10);
    auto lastAudioTime = clock::now();
    const float silencePeakThreshold = 1e-4f;

    // Cover extraction: tags are read once per song and cached to disk
    // next to the DB; QML loads the cached image directly.
    fs::path coverDirPath;
    int lastCoverSong = -1;
    TrackEvent cachedTrack;
    auto buildTrackEvent = [&](int songId, bool tentative, float conf) -> TrackEvent {
        TrackEvent t;
        t.valid = true;
        t.confidence = conf;
        t.tentative = tentative;
        SongInfo info = db.getSong(songId);
        t.title = info.songName;
        if (songId != lastCoverSong) {
            lastCoverSong = songId;
            AudioTags tags = readAudioTags(info.filePath);
            if (!tags.title.empty())  t.title = tags.title;
            if (!tags.artist.empty()) t.artist = tags.artist;
            if (!tags.album.empty())  t.album = tags.album;
            if (!tags.coverData.empty()) {
                if (coverDirPath.empty()) {
                    coverDirPath = pathutil::fromUtf8(dbPath).parent_path() / ".VJVision_covers";
                }
                std::error_code ec;
                fs::create_directories(coverDirPath, ec);
                fs::path cf = coverDirPath /
                    ("song_" + std::to_string(songId) + "." + tags.coverExt);
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, cf.c_str(), L"wb") == 0 && fp) {
                    fwrite(tags.coverData.data(), 1, tags.coverData.size(), fp);
                    fclose(fp);
                    t.coverPath = pathutil::toUtf8(cf);
                }
            }
            cachedTrack = t;
        } else {
            t.title = cachedTrack.title;
            t.artist = cachedTrack.artist;
            t.album = cachedTrack.album;
            t.coverPath = cachedTrack.coverPath;
        }
        return t;
    };

    // Log throttling: Tentative/MixHold events repeat every tick while the
    // phase lasts; log phase entry immediately, then progress at most every
    // 3 s. -2 = "no active candidate".
    int lastTentativeSong = -2;
    clock::time_point lastTentLog{};

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto now = clock::now();

        engine->setParams(matchParams());

        // --- spectrum @ ~30 fps ---
        if (now >= nextSpectrum) {
            nextSpectrum = now + std::chrono::milliseconds(33);
            float beatDt = std::chrono::duration<float>(now - lastBeatTick).count();
            beatDt = std::min(0.25f, std::max(0.005f, beatDt));
            lastBeatTick = now;
            if (cap.deviceLost()) {
                std::fill(bins.begin(), bins.end(), 0.f);
                beatTracker.reset();
                sink->onSpectrum(bins.data(), VIZ_SPECTRUM_BINS, 0.f);
                setStatus(VizStatus::Standby);
            } else {
                auto win = ring.readLatest(SpectrumAnalyzer::FFT_SIZE);
                float peak = 0.f;
                for (float s : win) { float a = s < 0.f ? -s : s; if (a > peak) peak = a; }
                if (peak > silencePeakThreshold) {
                    analyzer.analyze(win.data(), win.size(), bins.data(), rawBins.data());
                    beatTracker.process(rawBins.data(), VIZ_SPECTRUM_BINS, beatDt, false);
                    sink->onSpectrum(bins.data(), VIZ_SPECTRUM_BINS, peak,
                                     beatTracker.beat());
                    if (engine->currentSongId() < 0) setStatus(VizStatus::Listening);
                } else {
                    std::fill(bins.begin(), bins.end(), 0.f);
                    std::fill(rawBins.begin(), rawBins.end(), 0.f);
                    beatTracker.process(rawBins.data(), VIZ_SPECTRUM_BINS, beatDt, true);
                    sink->onSpectrum(bins.data(), VIZ_SPECTRUM_BINS, 0.f,
                                     beatTracker.beat());
                }
            }
        }

        // --- recognition cadence ---------------------------------------
        //   * no track locked yet ........ 0.5 s (fast first-track pickup)
        //   * stable confirmed track ..... 2 s (steady, low CPU)
        //   * transition (mix / FX) ...... 0.5 s until the bar fills back
        // Silent→audible edge fires recognition immediately regardless.
        float peak = cap.peakLevel();
        bool isSilent = peak < silencePeakThreshold;
        if (wasSilent && !isSilent) {
            // Edge: silent → audible — fire recognition NOW.
            nextTick = now;
        }
        wasSilent = isSilent;

        if (now < nextTick) continue;
        const bool trackLocked = engine->currentSongId() >= 0;
        const bool transitioning = engine->transitionActive();
        nextTick = now + ((trackLocked && !transitioning)
                              ? std::chrono::seconds(2)
                              : std::chrono::milliseconds(500));
        if (cap.deviceLost()) continue;

        double nowSec = std::chrono::duration<double>(now - tStart).count();
        if (peak < silencePeakThreshold) {
            FpResult empty;
            MatchTick mtSilent = engine->tick(empty, nowSec);
#ifdef VJVISION_CHARGE_ENGINE
            emit chargeUpdate(mtSilent.curSongId, mtSilent.curVotes, -1, 0,
                              static_cast<int>(mtSilent.event), 0.0);
#endif
            // Silence auto-timeout
            if (lastAudioTime + silenceTimeout <= now) {
                lastAudioTime = now;   // reset so we only fire once per timeout window
                // Reset the match engine — currentSongId_ 必须清零，否则
                // 下一首歌如果是同一张专辑或同一个 artist，engine->tick
                // 会返回 MatchEvent::None（因为 songId == currentSongId_），
                // 根本不发 track event → UI 永远停在待机界面！
                engine->reset();
                // Do NOT clear the track — we want the old title/artist to
                // stay on screen while contentProgress fades out (1.1s).
                // Setting hasTrack=false in QML triggers contentProgress
                // Behavior animation; track data is still bound under
                // opacity=0.0 during the fade, then next song detection
                // overwrites it.
                TrackEvent fadeOnly;
                fadeOnly.valid = false;
                sink->onTrack(fadeOnly);   // → hasTrack=false in QML → fade starts
                setStatus(VizStatus::Standby);
                fprintf(stderr, "[viz] Silence timeout → standby (engine reset, title preserved during fade).\n");
            }
            continue;
        }
        lastAudioTime = now;
        // Peak passed — now read the ring buffer for fingerprint matching.
        // Skip DB matching if no DB available (standby-only mode).
        FpResult result;
        size_t fpsCount = 0;
        const size_t windowSamples = transitioning ? windowSamplesTransition
                                                   : windowSamplesStable;
        if (hasDb) {
            // 4 phase grids at 0/1/2/3 quarter-hop offsets; each phase
            // slice is windowSamples long and ends at the latest sample.
            int phaseOff[kQueryPhases];
            for (int p = 0; p < kQueryPhases; ++p)
                phaseOff[p] = p * fp_params::HOP_SIZE / kQueryPhases;

            const size_t readSamples = windowSamples + kMaxPhaseOff;
            auto snap = ring.readLatest(readSamples);
            // Normalize against the capture block's LOCAL 0.5 s peak (as in
            // 2.0.x): a short window keeps quiet intros/passages loud enough
            // to produce matchable hashes — using the 12 s query window's own
            // peak (2.1.0 attempt) pinned gain low for up to 12 s after any
            // loud passage and made recognition dramatically slower. The 30x
            // (+30 dB) cap stays, so near-silence can no longer amplify the
            // noise floor 100x-1000x into spurious spectral peaks.
            float gain = (std::min)(30.f, 0.95f / peak);
            for (size_t i = 0; i < readSamples; ++i) {
                float v = snap[i] * gain;
                if (v > 1.f) v = 1.f;
                if (v < -1.f) v = -1.f;
                i16[i] = (int16_t)(v * 32767.f);
            }
            // 4-phase baseline scan: extract peaks per phase, generate
            // hashes, keep the strongest alignment. DT_QUANT in the hash
            // makes this robust to keylock-on tempo changes.
            std::vector<Peak> phasePeaks[kQueryPhases];
            const double winSec =
                (double)windowSamples / fp_params::SAMPLE_RATE;
            for (int p = 0; p < kQueryPhases; ++p) {
                int po = phaseOff[p];
                Spectrogram spec = computeSpectrogram(
                    i16.data() + po, windowSamples, fp_params::SAMPLE_RATE);
                phasePeaks[p] = extractPeaks(spec);
                auto fps = generateHashes(phasePeaks[p]);
                fpsCount = (std::max)(fpsCount, fps.size());
                if (fps.empty()) continue;
                auto hits = db.lookupHashes(fps);
                FpResult r = alignBestTail(fps, hits);
                if (!r.matched) continue;
                // Normalize the db/query delta into the invariant mapping
                // between the db file clock and the live clock:
                //   delta = (dbPos - livePos) + windowStart
                // Subtracting windowStart (≈ nowSec - windowSec) makes the
                // value comparable across ticks even as the window slides
                // and across 12 s / 6 s / 3 s-AGC query buffers. All phase
                // slices end on the same sample, so the constant is shared.
                r.offsetSec += winSec - nowSec;
                if (!result.matched ||
                    r.alignedVotes > result.alignedVotes ||
                    (r.alignedVotes == result.alignedVotes &&
                     r.inputConfidence > result.inputConfidence))
                    result = r;
            }

            // --- fader-low AGC tail pass (transitions only) ---
            // Peak extraction uses an ABSOLUTE dB floor and the DB was built
            // from full-scale files; an incoming track whose fader is still
            // low therefore yields far fewer query peaks and its confidence
            // bobs just under the switch gate. Re-fingerprint the last 3 s
            // normalized to THAT tail's own peak (up to 50x); the absolute
            // aligned-vote gate rejects noise-amplified tails.
            if (transitioning) ++transitionTickIdx;
            // 隔拍执行 AGC 尾通道：0.5s 节拍下每 1s 一次，控制 4 相位扫描
            // 的 CPU 增量；对齐票门限仍然拒绝噪声放大的尾段。
            if (transitioning && (transitionTickIdx % 2 == 0)) {
                constexpr size_t kAgcSamples = (size_t)fp_params::SAMPLE_RATE * 3;
                auto tail = ring.readLatest(kAgcSamples + kMaxPhaseOff);
                float tailPeak = 0.f;
                for (float s : tail) {
                    float a = s < 0.f ? -s : s;
                    if (a > tailPeak) tailPeak = a;
                }
                if (tailPeak >= silencePeakThreshold) {
                    float tailGain = (std::min)(50.f, 0.95f / tailPeak);
                    std::vector<int16_t> t16(kAgcSamples + kMaxPhaseOff);
                    for (size_t i = 0; i < t16.size(); ++i) {
                        float v = tail[i] * tailGain;
                        if (v > 1.f) v = 1.f;
                        if (v < -1.f) v = -1.f;
                        t16[i] = (int16_t)(v * 32767.f);
                    }
                    for (int po : phaseOff) {
                        Spectrogram spec = computeSpectrogram(
                            t16.data() + po, kAgcSamples,
                            fp_params::SAMPLE_RATE);
                        auto tpeaks = extractPeaks(spec);
                        auto tfps = generateHashes(tpeaks);
                        if ((int)tfps.size() < kMinSliceHashes) continue;
                        auto thits = db.lookupHashes(tfps);
                        FpResult tr = alignMatches(tfps, thits, (int)tfps.size());
                        if (tr.matched && tr.alignedVotes >= kMinAlignedVotes) {
                            tr.sliceSec = 3.0;
                            tr.localAgc = true;
                            // Same clock normalization as the main pass; this
                            // buffer covers the last 3 s for every phase.
                            tr.offsetSec += 3.0 - nowSec;
                            if (tr.inputConfidence > result.inputConfidence)
                                result = tr;
                        }
                    }
                }
            }
        }
        MatchTick mt = engine->tick(result, nowSec);

#ifdef VJVISION_CHARGE_ENGINE
        // 控制面板常驻电量窗
        {
            const bool hasCand = (mt.event == MatchEvent::Tentative ||
                                  mt.event == MatchEvent::MixHold);
            emit chargeUpdate(mt.curSongId, mt.curVotes,
                              hasCand ? mt.songId : -1,
                              hasCand ? mt.streakVotes : 0,
                              static_cast<int>(mt.event), mt.confidence);
        }
#endif

        // Debug: log every tick's confidence + event
        const char* evName = "None";
        switch (mt.event) {
            case MatchEvent::None:        evName = "None"; break;
            case MatchEvent::NoMatch:     evName = "NoMatch"; break;
            case MatchEvent::Noise:       evName = "Noise"; break;
            case MatchEvent::Tentative:   evName = "Tentative"; break;
            case MatchEvent::MixHold:     evName = "MixHold"; break;
            case MatchEvent::Confirmed:   evName = "CONFIRMED"; break;
        }
#ifdef VJVISION_CHARGE_ENGINE
        fprintf(stderr,
                "[viz] tick event=%-10s conf=%.4f songId=%d fps=%zu win=%zus%s"
                " votes=%d slice=%.1fs off=%.2f%s"
                " bar cur=%d/10 cand=%d/10\n",
                evName, mt.confidence, mt.songId, fpsCount,
                windowSamples / (size_t)fp_params::SAMPLE_RATE,
                transitioning ? " [transition]" : "",
                result.alignedVotes, result.sliceSec, result.offsetSec,
                result.localAgc ? " [agc]" : "",
                mt.curVotes, mt.streakVotes);
#else
        fprintf(stderr,
                "[viz] tick event=%-10s conf=%.4f songId=%d fps=%zu win=%zus%s"
                " votes=%d slice=%.1fs off=%.2f%s"
                " cluster=%dt/%dv\n",
                evName, mt.confidence, mt.songId, fpsCount,
                windowSamples / (size_t)fp_params::SAMPLE_RATE,
                transitioning ? " [transition]" : "",
                result.alignedVotes, result.sliceSec, result.offsetSec,
                result.localAgc ? " [agc]" : "",
                mt.streakTicks, mt.streakVotes);
#endif

        if (mt.event == MatchEvent::Confirmed) {
            // Only a CONFIRMED switch updates the display; QML cross-fades
            // from the previous song to this one.
            TrackEvent t = buildTrackEvent(mt.songId, false, mt.confidence);
            sink->onTrack(t);
            setStatus(VizStatus::Matching);
            char evTag[96];
#ifdef VJVISION_CHARGE_ENGINE
            std::snprintf(evTag, sizeof(evTag), " [bar %dv]",
                          mt.evidenceVotes);
#else
            if (mt.forceConfirmed)
                std::snprintf(evTag, sizeof(evTag), " [force]");
            else if (mt.evidenceConfirmed)
                std::snprintf(evTag, sizeof(evTag), " [evidence %dv]",
                              mt.evidenceVotes);
            else
                evTag[0] = '\0';
#endif
            char buf[640];
            std::snprintf(buf, sizeof(buf),
                          "CONFIRMED: '%s' conf=%.3f votes=%d slice=%.1fs%s%s",
                          t.title.c_str(), mt.confidence,
                          result.alignedVotes, result.sliceSec,
                          evTag,
                          t.coverPath.empty() ? "" : "  [cover]");
            log(buf);
        } else if (mt.event == MatchEvent::Tentative ||
                   mt.event == MatchEvent::MixHold) {
            // Candidate accumulating below the accept gate. Tentative = first
            // track (display stays on standby); MixHold = switch in progress
            // (display stays on the previous song). No onTrack, no visual
            // change either way. Log on candidate entry, then progress at
            // most every 3 s so the evidence cluster can be watched live.
            const bool firstTrack = (mt.event == MatchEvent::Tentative);
            if (!firstTrack) setStatus(VizStatus::Mixing);
            bool newCandidate = (mt.songId != lastTentativeSong);
            bool dueForProgress = (now - lastTentLog) >= std::chrono::seconds(3);
            if (newCandidate || dueForProgress) {
                lastTentativeSong = mt.songId;
                lastTentLog = now;
                SongInfo info = hasDb ? db.getSong(mt.songId) : SongInfo{};
                fprintf(stderr,
                        "[viz] candidate bar filling (%s):"
                        " '%s' conf=%.3f votes=%d slice=%.1fs%s\n",
                        firstTrack ? "standby" : "display held",
                        info.songName.c_str(), mt.confidence,
                        result.alignedVotes, result.sliceSec,
                        result.localAgc ? " [agc]" : "");
                char barTail[192];
#ifdef VJVISION_CHARGE_ENGINE
                std::snprintf(barTail, sizeof(barTail),
                              " bar cand=%d/10 (%dt) cur=%d/10 (%s)",
                              mt.streakVotes, mt.streakTicks, mt.curVotes,
                              firstTrack ? "standby, no display change"
                                         : "holding previous song");
#else
                std::snprintf(barTail, sizeof(barTail),
                              " cluster=%dv/%dt (%s)",
                              mt.streakVotes, mt.streakTicks,
                              firstTrack ? "standby, no display change"
                                         : "holding previous song");
#endif
                char buf[640];
                std::snprintf(buf, sizeof(buf),
                              "%s%s: '%s' conf=%.3f votes=%d slice=%.1fs%s%s",
                              firstTrack ? "candidate" : "mix",
                              newCandidate ? "" : " …",
                              info.songName.c_str(), mt.confidence,
                              result.alignedVotes, result.sliceSec,
                              result.localAgc ? " [agc]" : "",
                              barTail);
                log(buf);
            }
        }
        if (mt.event != MatchEvent::Tentative &&
            mt.event != MatchEvent::MixHold) {
            lastTentativeSong = -2;
        }
    }

    cap.stop();
    log("Visualizer stopped.");
}

} // namespace vj
