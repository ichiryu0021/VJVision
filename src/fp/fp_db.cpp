#include "fp_db.h"
#include <sqlite3.h>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

namespace vj {

FpDb::FpDb() = default;

FpDb::~FpDb() { close(); }

bool FpDb::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        fprintf(stderr, "SQLite open failed: %s\n", sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA busy_timeout=30000;");
    exec("PRAGMA foreign_keys=ON;");

    exec("CREATE TABLE IF NOT EXISTS songs ("
         "song_id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "song_name TEXT NOT NULL,"
         "fingerprinted INTEGER NOT NULL DEFAULT 0,"
         "file_sha1 TEXT NOT NULL DEFAULT '',"
         "total_hashes INTEGER NOT NULL DEFAULT 0,"
         "date_created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "date_modified TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");

    exec("CREATE TABLE IF NOT EXISTS fingerprints ("
         "hash TEXT NOT NULL,"
         "song_id INTEGER NOT NULL,"
         "offset INTEGER NOT NULL,"
         "date_created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "date_modified TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "UNIQUE(song_id, offset, hash));");

    exec("CREATE INDEX IF NOT EXISTS ix_fingerprints_hash ON fingerprints(hash);");

    exec("CREATE TABLE IF NOT EXISTS song_paths ("
         "song_id INTEGER PRIMARY KEY,"
         "file_path TEXT NOT NULL);");
    return true;
}

void FpDb::close() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

void FpDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL exec error: %s\n", err);
        sqlite3_free(err);
    }
}

int FpDb::insertSong(const std::string& name, const std::string& sha1,
                     int totalHashes, const std::string& filePath) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO songs (song_name, file_sha1, total_hashes) VALUES (?, ?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sha1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, totalHashes);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    int songId = (int)sqlite3_last_insert_rowid(db_);

    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO song_paths (song_id, file_path) VALUES (?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, songId);
    sqlite3_bind_text(stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db_, "UPDATE songs SET fingerprinted=1 WHERE song_id=?;",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, songId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return songId;
}

void FpDb::insertHashes(int songId, const std::vector<Fingerprint>& hashes) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO fingerprints (song_id, hash, offset) VALUES (?, ?, ?);",
        -1, &stmt, nullptr);
    for (const auto& fp : hashes) {
        sqlite3_bind_int(stmt, 1, songId);
        sqlite3_bind_text(stmt, 2, fp.hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, fp.offset);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void FpDb::beginTx() { exec("BEGIN;"); }
void FpDb::commitTx() { exec("COMMIT;"); }

void FpDb::setCacheSize(int mb) {
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA cache_size=%d;", -mb * 1024);
    exec(sql);
}

void FpDb::dropIndexForBulk() {
    exec("DROP INDEX IF EXISTS ix_fingerprints_hash;");
}

void FpDb::recreateIndexAfterBulk() {
    exec("CREATE INDEX IF NOT EXISTS ix_fingerprints_hash ON fingerprints(hash);");
}

std::vector<HashHit> FpDb::lookupHashes(const std::vector<Fingerprint>& query) {
    std::vector<HashHit> hits;
    if (query.empty()) return hits;

    // Build a map from hash -> list of query offsets (same hash can appear
    // at multiple offsets in the query).
    std::unordered_map<std::string, std::vector<int>> hashToOffsets;
    for (const auto& fp : query)
        hashToOffsets[fp.hash].push_back(fp.offset);

    // Collect unique hashes for the IN clause.
    std::vector<std::string> uniqueHashes;
    uniqueHashes.reserve(hashToOffsets.size());
    for (const auto& [h, _] : hashToOffsets) uniqueHashes.push_back(h);

    std::string sql = "SELECT hash, song_id, offset FROM fingerprints WHERE hash IN (";
    for (size_t i = 0; i < uniqueHashes.size(); ++i) {
        if (i) sql += ',';
        sql += "?";
    }
    sql += ");";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < uniqueHashes.size(); ++i)
        sqlite3_bind_text(stmt, (int)(i + 1), uniqueHashes[i].c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* h = sqlite3_column_text(stmt, 0);
        int songId = sqlite3_column_int(stmt, 1);
        int dbOffset = sqlite3_column_int(stmt, 2);
        std::string hashStr = h ? (const char*)h : "";
        // Look up the query offset(s) for this hash.
        auto it = hashToOffsets.find(hashStr);
        if (it != hashToOffsets.end()) {
            for (int qOff : it->second) {
                HashHit hit;
                hit.songId = songId;
                hit.dbOffset = dbOffset;
                hit.queryOffset = qOff;
                hits.push_back(hit);
            }
        }
    }
    sqlite3_finalize(stmt);
    return hits;
}

SongInfo FpDb::getSong(int songId) {
    SongInfo info{};
    info.songId = songId;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT s.song_name, s.total_hashes, p.file_path FROM songs s "
        "LEFT JOIN song_paths p ON s.song_id=p.song_id WHERE s.song_id=?;",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, songId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 0);
        const unsigned char* path = sqlite3_column_text(stmt, 2);
        if (name) info.songName = (const char*)name;
        if (path) info.filePath = (const char*)path;
        info.totalHashes = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return info;
}

std::vector<SongInfo> FpDb::listSongs() {
    std::vector<SongInfo> songs;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT s.song_id, s.song_name, s.total_hashes, p.file_path FROM songs s "
        "LEFT JOIN song_paths p ON s.song_id=p.song_id WHERE s.fingerprinted=1;",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SongInfo info{};
        info.songId = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        const unsigned char* path = sqlite3_column_text(stmt, 3);
        if (name) info.songName = (const char*)name;
        if (path) info.filePath = (const char*)path;
        info.totalHashes = sqlite3_column_int(stmt, 2);
        songs.push_back(std::move(info));
    }
    sqlite3_finalize(stmt);
    return songs;
}

int FpDb::countFingerprints() {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM fingerprints;", -1, &stmt, nullptr);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool FpDb::songExistsByPath(const std::string& filePath) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT 1 FROM song_paths WHERE file_path=? LIMIT 1;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

void FpDb::clear() {
    exec("DROP TABLE IF EXISTS fingerprints;");
    exec("DROP TABLE IF EXISTS songs;");
    exec("DROP TABLE IF EXISTS song_paths;");
    exec("CREATE TABLE IF NOT EXISTS songs ("
         "song_id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "song_name TEXT NOT NULL,"
         "fingerprinted INTEGER NOT NULL DEFAULT 0,"
         "file_sha1 TEXT NOT NULL DEFAULT '',"
         "total_hashes INTEGER NOT NULL DEFAULT 0,"
         "date_created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "date_modified TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    exec("CREATE TABLE IF NOT EXISTS fingerprints ("
         "hash TEXT NOT NULL,"
         "song_id INTEGER NOT NULL,"
         "offset INTEGER NOT NULL,"
         "date_created TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "date_modified TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
         "UNIQUE(song_id, offset, hash));");
    exec("CREATE INDEX IF NOT EXISTS ix_fingerprints_hash ON fingerprints(hash);");
    exec("CREATE TABLE IF NOT EXISTS song_paths ("
         "song_id INTEGER PRIMARY KEY,"
         "file_path TEXT NOT NULL);");
}

void FpDb::vacuum() { exec("VACUUM;"); }

} // namespace vj
