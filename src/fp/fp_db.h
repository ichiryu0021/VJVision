// SQLite fingerprint storage — songs + fingerprints tables, WAL mode.
// Schema mirrors Python dejavu_sqlite.py.
#pragma once
#include "fingerprint.h"
#include <string>
#include <vector>

struct sqlite3;

namespace vj {

struct SongInfo {
    int songId;
    std::string songName;
    std::string filePath;
    int totalHashes;
};

class FpDb {
public:
    FpDb();
    ~FpDb();

    bool open(const std::string& path);
    void close();

    // Insert a song and return its song_id.
    int insertSong(const std::string& name, const std::string& sha1, int totalHashes,
                   const std::string& filePath);

    // Bulk-insert fingerprints for a song (transaction-wrapped).
    void insertHashes(int songId, const std::vector<Fingerprint>& hashes);

    // Look up all (song_id, db_offset) rows matching the given query hashes.
    // Each returned hit also carries the query_offset of the hash that matched it,
    // so the caller can compute delta = db_offset - query_offset for alignment.
    std::vector<HashHit> lookupHashes(const std::vector<Fingerprint>& query);

    SongInfo getSong(int songId);
    std::vector<SongInfo> listSongs();
    int countFingerprints();

    // True if a song row already references this file path (skip on reindex).
    bool songExistsByPath(const std::string& filePath);

    void clear();
    void vacuum();

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
};

} // namespace vj
