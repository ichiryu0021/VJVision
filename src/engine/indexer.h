// Parallel library indexer.
//
// Workers decode + fingerprint files (CPU-bound, no shared state); the
// orchestrator thread writes songs/hashes to SQLite serially (one writer,
// so WAL stays contention-free and memory stays flat). Files that fail
// in the parallel pass are retried once single-threaded — transient
// decoder/IO hiccups then recover without a full re-run.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace vj {

class FpDb;

struct IndexProgress {
    int done = 0;     // files processed so far (ok + failed + skipped)
    int total = 0;    // candidate files discovered
    std::string info; // last file name / status line
};

struct IndexResult {
    int totalFiles = 0;
    int indexedOk = 0;
    int skipped = 0;  // already present in song_paths
    int failed = 0;
    std::vector<std::string> failedPaths;
};

class Indexer {
public:
    using ProgressCb = std::function<void(const IndexProgress&)>;

    // workers <= 0 → auto: min(hardware_concurrency, 8).
    IndexResult indexDirectory(const std::string& dir, FpDb& db,
                               int workers, ProgressCb cb);
};

} // namespace vj
