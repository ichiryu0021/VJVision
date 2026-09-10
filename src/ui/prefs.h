// Persisted user preferences. Portable by design: the JSON file lives
// next to the executable (USB-stick deployment must not touch the
// registry / AppData).
#pragma once
#include "../engine/match_engine.h"
#include <QString>

namespace vj {

struct Prefs {
    QString dbPath;            // SQLite fingerprint DB
    QString musicDir;          // last indexed music directory
    int deviceIdx = -1;        // capture endpoint (-1 = default loopback)
    int screenIdx = -1;        // fullscreen screen (-1 = primary)
    QString language = "zh";   // "zh" | "en"
    MatchParams match;         // live recognition thresholds

    // <exe dir>/VJVision_prefs.json
    static QString defaultPath();

    static Prefs load();       // missing/corrupt file → built-in defaults
    void save() const;
};

} // namespace vj
