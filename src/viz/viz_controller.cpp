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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace vj {

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

bool VizController::start(const std::string& dbPath, int deviceIndex) {
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
    worker_ = std::thread(&VizController::workerFunc, this, dbPath, deviceIndex);
    emit logMessage(QStringLiteral("Visualizer started (device=%1, windowed — drag + F for fullscreen)")
                        .arg(deviceIndex));
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

void VizController::workerFunc(std::string dbPath, int deviceIndex) {
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
    cap.open(deviceIndex);
    if (!cap.start(&ring)) {
        log("Failed to start audio capture (device unavailable).");
        return;
    }

    SpectrumAnalyzer analyzer(fp_params::SAMPLE_RATE);
    BeatTracker beatTracker;
    MatchEngine engine(matchParams());
    std::vector<float> bins(VIZ_SPECTRUM_BINS, 0.f);
    std::vector<float> rawBins(VIZ_SPECTRUM_BINS, 0.f);

    const int windowSec = 12;
    const size_t windowSamples = (size_t)fp_params::SAMPLE_RATE * windowSec;
    std::vector<int16_t> i16(windowSamples);

    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();
    auto nextTick = clock::now();   // start immediately — don't wait 3s
    auto nextSpectrum = clock::now();
    auto lastBeatTick = clock::now();

    // Track silent→audible edge so we can fire recognition immediately
    // when music comes back (don't wait for the next 2s tick slot).
    bool wasSilent = true;

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

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto now = clock::now();

        engine.setParams(matchParams());

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
                    if (engine.currentSongId() < 0) setStatus(VizStatus::Listening);
                } else {
                    std::fill(bins.begin(), bins.end(), 0.f);
                    std::fill(rawBins.begin(), rawBins.end(), 0.f);
                    beatTracker.process(rawBins.data(), VIZ_SPECTRUM_BINS, beatDt, true);
                    sink->onSpectrum(bins.data(), VIZ_SPECTRUM_BINS, 0.f,
                                     beatTracker.beat());
                }
            }
        }

        // --- recognition @ 1 / 2 s (4s was too slow for real-time feel) ---
        // Check peak FIRST so we can detect silent→audible edge and fire
        // recognition immediately when music returns (don't wait for next
        // 2s tick slot).
        float peak = cap.peakLevel();
        bool isSilent = peak < silencePeakThreshold;
        if (wasSilent && !isSilent) {
            // Edge: silent → audible — fire recognition NOW.
            nextTick = now;
        }
        wasSilent = isSilent;

        if (now < nextTick) continue;
        nextTick = now + std::chrono::seconds(2);
        if (cap.deviceLost()) continue;

        double nowSec = std::chrono::duration<double>(now - tStart).count();
        if (peak < silencePeakThreshold) {
            FpResult empty;
            engine.tick(empty, nowSec);
            // Silence auto-timeout
            if (lastAudioTime + silenceTimeout <= now) {
                lastAudioTime = now;   // reset so we only fire once per timeout window
                // Reset the match engine — currentSongId_ 必须清零，否则
                // 下一首歌如果是同一张专辑或同一个 artist，engine.tick
                // 会返回 MatchEvent::None（因为 songId == currentSongId_），
                // 根本不发 track event → UI 永远停在待机界面！
                engine.reset();
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
        if (hasDb) {
            auto snap = ring.readLatest(windowSamples);
            float gain = 0.95f / peak;
            for (size_t i = 0; i < windowSamples; ++i) {
                float v = snap[i] * gain;
                if (v > 1.f) v = 1.f;
                if (v < -1.f) v = -1.f;
                i16[i] = (int16_t)(v * 32767.f);
            }
            auto fps = fingerprintSignal(i16.data(), windowSamples, fp_params::SAMPLE_RATE);
            fpsCount = fps.size();
            if (!fps.empty()) {
                auto hits = db.lookupHashes(fps);
                result = alignMatches(fps, hits, (int)fps.size());
            }
        }
        MatchTick mt = engine.tick(result, nowSec);

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
        fprintf(stderr, "[viz] tick event=%-10s conf=%.4f songId=%d fps=%zu\n",
                evName, mt.confidence, mt.songId, fpsCount);

        if (mt.event == MatchEvent::Confirmed || mt.event == MatchEvent::Tentative) {
            bool tentative = (mt.event == MatchEvent::Tentative);
            TrackEvent t = buildTrackEvent(mt.songId, tentative, mt.confidence);
            sink->onTrack(t);
            if (!tentative) setStatus(VizStatus::Matching);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s: '%s' conf=%.3f%s",
                          tentative ? "tentative" : "CONFIRMED",
                          t.title.c_str(), mt.confidence,
                          t.coverPath.empty() ? "" : "  [cover]");
            log(buf);
        } else if (mt.event == MatchEvent::MixHold) {
            setStatus(VizStatus::Mixing);
        }
    }

    cap.stop();
    log("Visualizer stopped.");
}

} // namespace vj
