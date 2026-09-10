#include "indexer.h"
#include "../fp/fp_db.h"
#include "../fp/pipeline.h"
#include "../util/audio_file.h"
#include "../util/path_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace vj {

namespace {

struct JobResult {
    std::string path;
    std::string name;
    std::vector<Fingerprint> fps;
    std::string error;
    bool ok = false;
};

// Decode + fingerprint one file. Runs on worker threads; touches no DB.
JobResult processFile(const std::string& path) {
    JobResult r;
    r.path = path;
    try {
        LoadedAudio a = loadAudioFile(path);
        r.fps = fingerprintSignal(a.monoSamples.data(), a.monoSamples.size(),
                                  a.sampleRate);
        r.name = pathutil::toUtf8(pathutil::fromUtf8(path).stem());
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.ok = false;
    }
    return r;
}

} // namespace

IndexResult Indexer::indexDirectory(const std::string& dir, FpDb& db,
                                    int workers, ProgressCb cb) {
    IndexResult summary;

    // --- 1. Discover candidate files -------------------------------
    // dir is UTF-8; on Windows narrow-string fs APIs use ANSI, so cross
    // through u8path and emit paths back as UTF-8.
    std::vector<std::string> files;
    std::error_code ec;
    fs::path rootPath = pathutil::fromUtf8(dir);
    for (const auto& entry : fs::recursive_directory_iterator(rootPath, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file()) continue;
        std::string ext = pathutil::toUtf8(entry.path().extension());
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".wav" || ext == ".flac" || ext == ".mp3") {
            // Normalize to native separators: the input dir may contain
            // forward slashes (JSON prefs / typed paths), and concatenating
            // it with native separator entries yields mixed paths that
            // never match previously stored song_paths (skip-by-path
            // depends on exact equality).
            fs::path native = entry.path();
            native.make_preferred();
            files.push_back(pathutil::toUtf8(native));
        }
    }
    summary.totalFiles = (int)files.size();

    if (workers <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        workers = hc ? (int)hc : 4;
        if (workers > 8) workers = 8;
    }
    if (workers > (int)files.size()) workers = (int)files.size();
    if (workers < 1) workers = 1;

    // --- 2. Skip files already indexed -----------------------------
    std::vector<std::string> todo;
    for (const auto& f : files) {
        if (db.songExistsByPath(f)) {
            ++summary.skipped;
        } else {
            todo.push_back(f);
        }
    }
    if (cb) cb({0, summary.totalFiles,
                "Discovered " + std::to_string(summary.totalFiles) +
                " file(s), " + std::to_string(summary.skipped) + " already indexed, " +
                std::to_string(todo.size()) + " to process with " +
                std::to_string(workers) + " worker(s)"});

    // --- 3. Parallel fingerprinting, serial DB writes --------------
    std::atomic<size_t> nextIdx{0};
    std::mutex resultMtx;
    std::condition_variable resultCv;
    std::vector<JobResult> results;
    std::atomic<int> finished{0};
    const int jobCount = (int)todo.size();

    auto worker = [&]() {
        while (true) {
            size_t idx = nextIdx.fetch_add(1);
            if (idx >= todo.size()) break;
            JobResult r = processFile(todo[idx]);
            {
                std::lock_guard<std::mutex> lk(resultMtx);
                results.push_back(std::move(r));
            }
            resultCv.notify_one();
        }
        finished.fetch_add(1);
        resultCv.notify_all();
    };

    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w) pool.emplace_back(worker);

    int consumed = 0;
    while (consumed < jobCount) {
        std::vector<JobResult> batch;
        {
            std::unique_lock<std::mutex> lk(resultMtx);
            resultCv.wait(lk, [&]() {
                return !results.empty() || finished.load() == workers;
            });
            batch.swap(results);
        }
        for (auto& r : batch) {
            ++consumed;
            if (r.ok) {
                int songId = db.insertSong(r.name, "", (int)r.fps.size(), r.path);
                db.insertHashes(songId, r.fps);
                ++summary.indexedOk;
                if (cb) cb({consumed + summary.skipped, summary.totalFiles,
                            "[OK] " + r.name + " -> " + std::to_string(r.fps.size()) +
                            " hashes, song_id=" + std::to_string(songId)});
            } else {
                ++summary.failed;
                summary.failedPaths.push_back(r.path);
                if (cb) cb({consumed + summary.skipped, summary.totalFiles,
                            "[FAIL] " + r.path + ": " + r.error});
            }
        }
    }
    for (auto& t : pool) t.join();

    // --- 4. Single-threaded retry for failed files -----------------
    if (!summary.failedPaths.empty()) {
        std::vector<std::string> retry = std::move(summary.failedPaths);
        summary.failedPaths.clear();
        summary.failed = 0;
        if (cb) cb({summary.totalFiles, summary.totalFiles,
                    "Retrying " + std::to_string(retry.size()) +
                    " failed file(s) single-threaded..."});
        for (const auto& path : retry) {
            JobResult r = processFile(path);
            if (r.ok) {
                int songId = db.insertSong(r.name, "", (int)r.fps.size(), r.path);
                db.insertHashes(songId, r.fps);
                ++summary.indexedOk;
                if (cb) cb({summary.totalFiles, summary.totalFiles,
                            "[RETRY OK] " + r.name});
            } else {
                ++summary.failed;
                summary.failedPaths.push_back(path);
                if (cb) cb({summary.totalFiles, summary.totalFiles,
                            "[RETRY FAIL] " + path + ": " + r.error});
            }
        }
    }

    return summary;
}

} // namespace vj
