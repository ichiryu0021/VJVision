// Hash-voting alignment — groups DB hits by (song_id, db_offset - query_offset)
// and picks the song with the most aligned votes (input_confidence).
#pragma once
#include "fingerprint.h"
#include <vector>

namespace vj {

// Align query fingerprints against DB hits.
// queryHashes: the query fingerprints (with their offsets).
// hits: all (song_id, db_offset) rows matching the query hashes.
// queryHashCount: total number of query hashes (for confidence normalization).
FpResult alignMatches(const std::vector<Fingerprint>& query,
                      const std::vector<HashHit>& hits,
                      int queryHashCount);

} // namespace vj
