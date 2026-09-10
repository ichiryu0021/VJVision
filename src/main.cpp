// VJVision — audio fingerprinting CLI.
//
// Usage:
//   VJVision index <directory> <db_path>     Index all .wav files in directory
//   VJVision query <wav_file> <db_path>      Query a wav clip against the DB
//   VJVision list <db_path>                  List indexed songs
//   VJVision devices                         List audio devices (loopback first)
//   VJVision listen <db_path> [device_idx]   Real-time recognition from system
//                                            loopback (default device if idx -1)
//
#include "fp/fingerprint.h"
#include "fp/pipeline.h"
#include "fp/fp_db.h"
#include "fp/align.h"
#include "util/wav.h"
#include "audio/wasapi_capture.h"
#include "audio/ring_buffer.h"
#include "audio/spectrum.h"
#include "engine/match_engine.h"
#include "engine/indexer.h"
#include "viz/viz_events.h"
#include "viz/ipc_pipe.h"
#include "util/tags.h"
#include "util/path_util.h"
#include "util/audio_file.h"
#ifdef VJVC_WITH_QT
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QScreen>
#include "viz/qt_viz.h"
#include "viz/viz_controller.h"
#include "ui/control_panel.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;
using namespace vj;

static void indexDirectory(const std::string& dir, const std::string& dbPath) {
    FpDb db;
    if (!db.open(dbPath)) { fprintf(stderr, "Cannot open DB %s\n", dbPath.c_str()); return; }

    Indexer indexer;
    auto t0 = std::chrono::steady_clock::now();
    IndexResult r = indexer.indexDirectory(dir, db, 0, [](const IndexProgress& p) {
        printf("[%d/%d] %s\n", p.done, p.total, p.info.c_str());
    });
    auto sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    printf("Done in %.1fs: %d indexed, %d skipped (already in DB), %d failed. "
           "Total fingerprints: %d\n",
           sec, r.indexedOk, r.skipped, r.failed, db.countFingerprints());
    for (const auto& f : r.failedPaths)
        fprintf(stderr, "  FAILED: %s\n", f.c_str());
}

static void queryWav(const std::string& wavPath, const std::string& dbPath) {
    FpDb db;
    if (!db.open(dbPath)) { fprintf(stderr, "Cannot open DB %s\n", dbPath.c_str()); return; }

    LoadedAudio audio;
    try {
        audio = loadAudioFile(wavPath);
    } catch (const std::exception& e) {
        fprintf(stderr, "Failed to load %s: %s\n", wavPath.c_str(), e.what());
        return;
    }
    auto queryFps = fingerprintSignal(audio.monoSamples.data(), audio.monoSamples.size(),
                                      audio.sampleRate);
    printf("Query: %s -> %d hashes\n", wavPath.c_str(), (int)queryFps.size());

    if (queryFps.empty()) {
        printf("No hashes generated (silence?)\n");
        return;
    }

    auto hits = db.lookupHashes(queryFps);
    printf("DB hits: %zu\n", hits.size());

    FpResult result = alignMatches(queryFps, hits, (int)queryFps.size());
    if (result.songId >= 0) {
        SongInfo info = db.getSong(result.songId);
        printf("MATCH: song_id=%d name='%s' confidence=%.3f offset=%.1fs matched=%s\n",
               result.songId, info.songName.c_str(), result.inputConfidence,
               result.offsetSec, result.matched ? "YES" : "NO");
    } else {
        printf("NO MATCH\n");
    }
}

static void listSongs(const std::string& dbPath) {
    FpDb db;
    if (!db.open(dbPath)) { fprintf(stderr, "Cannot open DB %s\n", dbPath.c_str()); return; }
    auto songs = db.listSongs();
    printf("%d song(s) indexed, %d total fingerprints\n",
           (int)songs.size(), db.countFingerprints());
    for (const auto& s : songs)
        printf("  [%d] %s (%d hashes) %s\n", s.songId, s.songName.c_str(),
               s.totalHashes, s.filePath.c_str());
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

static void listDevicesCmd() {
    auto devs = WasapiCapture::listDevices();
    if (devs.empty()) {
        printf("No active audio devices found.\n");
        return;
    }
    printf("%zu device(s):\n", devs.size());
    for (const auto& d : devs) {
        printf("  [%d] %s%s\n", d.index, wideToUtf8(d.name).c_str(),
               d.isLoopback ? "  (loopback / system mix)" : "  (input)");
    }
    printf("Use index -1 (default) or one of the above with 'listen'.\n");
}

static std::atomic<bool> g_listenRunning{true};
static BOOL WINAPI listenCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_listenRunning.store(false);
        return TRUE;
    }
    return FALSE;
}

static int listenCmd(const std::string& dbPath, int deviceIndex) {
    FpDb db;
    if (!db.open(dbPath)) { fprintf(stderr, "Cannot open DB %s\n", dbPath.c_str()); return 1; }
    if (db.listSongs().empty()) {
        fprintf(stderr, "DB has no indexed songs — run 'index' first.\n");
        return 1;
    }

    RingBuffer ring((size_t)fp_params::SAMPLE_RATE * 30); // 30 s buffer
    WasapiCapture cap;
    cap.open(deviceIndex);
    if (!cap.start(&ring)) {
        fprintf(stderr, "Failed to start capture.\n");
        return 1;
    }

    SetConsoleCtrlHandler(listenCtrlHandler, TRUE);
    printf("Listening on %s... (Ctrl+C to stop)\n",
           deviceIndex < 0 ? "default loopback device" : "selected device");

    const int windowSec = 12;   // recognition window
    const int hopSec = 4;       // re-query interval
    const size_t windowSamples = (size_t)fp_params::SAMPLE_RATE * windowSec;
    std::vector<int16_t> i16(windowSamples);

    MatchEngine engine;
    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();

    auto nextTick = clock::now() + std::chrono::seconds(3);
    while (g_listenRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto now = clock::now();
        if (now < nextTick) continue;
        nextTick = now + std::chrono::seconds(hopSec);

        if (cap.deviceLost()) {
            printf("[status] audio device lost, reconnecting... (peak=%.3f)\n",
                   cap.peakLevel());
            continue;
        }

        std::vector<float> snap = ring.readLatest(windowSamples);
        // Peak normalization to 0.95 full-scale (matches dejavu query path).
        float peak = 0.f;
        for (float s : snap) {
            float a = s < 0.f ? -s : s;
            if (a > peak) peak = a;
        }
        double nowSec = std::chrono::duration<double>(now - tStart).count();
        if (peak < 1e-5f) {
            // Feed the engine an empty result so pending/tentative state
            // ages out during silence, but stay quiet on screen.
            FpResult empty;
            engine.tick(empty, nowSec);
            continue;
        }
        float gain = 0.95f / peak;
        for (size_t i = 0; i < windowSamples; ++i) {
            float v = snap[i] * gain;
            if (v > 1.f) v = 1.f;
            if (v < -1.f) v = -1.f;
            i16[i] = (int16_t)(v * 32767.f);
        }

        auto fps = fingerprintSignal(i16.data(), windowSamples, fp_params::SAMPLE_RATE);
        FpResult result;
        if (!fps.empty()) {
            auto hits = db.lookupHashes(fps);
            result = alignMatches(fps, hits, (int)fps.size());
        }

        MatchTick mt = engine.tick(result, nowSec);
        switch (mt.event) {
            case MatchEvent::Confirmed: {
                SongInfo info = db.getSong(mt.songId);
                printf("[%.0fs] *** MATCH: '%s'  conf=%.3f  offset=%.1fs "
                       "(hashes=%d) ***\n",
                       nowSec, info.songName.c_str(), mt.confidence, mt.offsetSec,
                       (int)fps.size());
                break;
            }
            case MatchEvent::Tentative: {
                SongInfo info = db.getSong(mt.songId);
                printf("[%.0fs] ~ tentative: '%s'  conf=%.3f (pulsing, unconfirmed)\n",
                       nowSec, info.songName.c_str(), mt.confidence);
                break;
            }
            case MatchEvent::MixHold:
                printf("[%.0fs] [mix] cross-fade detected, holding current track "
                       "(conf=%.3f)\n", nowSec, mt.confidence);
                break;
            case MatchEvent::Noise:
                printf("[%.0fs] . noise floor (conf=%.3f)\n", nowSec, mt.confidence);
                break;
            case MatchEvent::NoMatch:
                // Silence / no hashes — stay quiet to avoid log spam.
                break;
            case MatchEvent::None:
                // Pending confirmation streak or re-confirm of current track.
                if (mt.confidence >= 0.13f) {
                    printf("[%.0fs]   pending/hold  conf=%.3f  (current song_id=%d)\n",
                           nowSec, mt.confidence, engine.currentSongId());
                }
                break;
        }
    }

    printf("\nStopping...\n");
    cap.stop();
    SetConsoleCtrlHandler(listenCtrlHandler, FALSE);
    return 0;
}

// ------------------------------------------------------------------
// viz / panel: Qt front-ends (M3 / M4)
// ------------------------------------------------------------------
#ifdef VJVC_WITH_QT
// Standalone fullscreen visualizer (same session class as the panel).
static int vizCmd(int argc, char** argv, const std::string& dbPath,
                  int deviceIndex, int screenIndex) {
    QApplication app(argc, argv);

    VizController ctl;
    QObject::connect(&ctl, &VizController::logMessage, &app,
        [](const QString& line) {
            printf("%s\n", line.toUtf8().constData());
        }, Qt::DirectConnection);
    if (!ctl.start(dbPath, deviceIndex, screenIndex)) return 1;
    // In standalone mode, closing the visualizer (Escape) exits the app.
    QObject::connect(&ctl, &VizController::sessionStopped,
                     &app, [&] { app.quit(); });
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&] { ctl.stop(); });
    return app.exec();
}

// M4: main control panel. No command-line argument launches this.
static int panelCmd(int argc, char** argv) {
    QApplication app(argc, argv);
    ControlPanel panel;
    panel.show();
    return app.exec();
}
#endif

static int listScreensCmd(int argc, char** argv) {
#ifdef VJVC_WITH_QT
    QGuiApplication app(argc, argv);
    const auto screens = QGuiApplication::screens();
    QScreen* primary = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        QRect g = s->geometry();
        printf("[%d] %s %dx%d @ (%d,%d) DPR=%.2f%s\n",
               i, s->name().toUtf8().constData(),
               g.width(), g.height(), g.x(), g.y(),
               s->devicePixelRatio(),
               s == primary ? "  <- primary (viz default)" : "");
    }
    printf("Use: VJVision viz <db> [device_idx] <screen_idx>\n");
    return 0;
#else
    (void)argc; (void)argv;
    fprintf(stderr, "This build has no Qt support (-DVJVC_WITH_QT=OFF).\n");
    return 1;
#endif
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: output survives kill
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Rebuild argv as UTF-8 (CRT narrow argv uses the ANSI codepage on
    // Windows; library paths / DB paths with Chinese characters must survive).
    std::vector<std::string> argStore;
    std::vector<char*> uargv;
    {
        int wargc = 0;
        LPWSTR* wargs = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargs) {
            for (int i = 0; i < wargc; ++i)
                argStore.push_back(pathutil::wideToUtf8(wargs[i]));
            LocalFree(wargs);
        } else {
            for (int i = 0; i < argc; ++i) argStore.emplace_back(argv[i]);
        }
        uargv.reserve(argStore.size());
        for (auto& s : argStore) uargv.push_back(s.data());
        argc = (int)argStore.size();
        argv = uargv.data();
    }

    if (argc < 2) {
        // No arguments → graphical control panel (M4). Console usage is
        // available via the "help" command.
#ifdef VJVC_WITH_QT
        return panelCmd(argc, argv);
#else
        fprintf(stderr, "This build has no Qt support; run '%s help'.\n", argv[0]);
        return 1;
#endif
    }
    std::string cmd = argv[1];
    if (cmd == "help") {
        printf("VJVision — audio fingerprint recognition\n"
               "Usage:\n"
               "  %s                       Launch control panel (GUI)\n"
               "  %s panel                 Launch control panel (GUI)\n"
               "  %s index <directory> <db_path>\n"
               "  %s query <wav_file> <db_path>\n"
               "  %s list <db_path>\n"
               "  %s devices\n"
               "  %s screens\n"
               "  %s listen <db_path> [device_idx]\n"
               "  %s viz <db_path> [device_idx] [screen_idx]\n",
               argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
               argv[0], argv[0], argv[0]);
        return 0;
    }
    if (cmd == "index" && argc == 4) {
        indexDirectory(argv[2], argv[3]);
    } else if (cmd == "query" && argc == 4) {
        queryWav(argv[2], argv[3]);
    } else if (cmd == "list" && argc == 3) {
        listSongs(argv[2]);
    } else if (cmd == "devices" && argc == 2) {
        listDevicesCmd();
    } else if (cmd == "screens" && argc == 2) {
        return listScreensCmd(argc, argv);
    } else if (cmd == "panel" && argc == 2) {
#ifdef VJVC_WITH_QT
        return panelCmd(argc, argv);
#else
        fprintf(stderr, "This build has no Qt support (-DVJVC_WITH_QT=OFF).\n");
        return 1;
#endif
    } else if (cmd == "listen" && (argc == 3 || argc == 4)) {
        int idx = -1;
        if (argc == 4) idx = std::atoi(argv[3]);
        return listenCmd(argv[2], idx);
    } else if (cmd == "viz" && (argc >= 3 && argc <= 5)) {
#ifdef VJVC_WITH_QT
        int dev = -1, scr = -1;
        if (argc >= 4) dev = std::atoi(argv[3]);
        if (argc >= 5) scr = std::atoi(argv[4]);
        return vizCmd(argc, argv, argv[2], dev, scr);
#else
        fprintf(stderr, "This build has no Qt support (-DVJVC_WITH_QT=OFF).\n");
        return 1;
#endif
    } else {
        fprintf(stderr, "Unknown command or wrong args\n");
        return 1;
    }
    return 0;
}
