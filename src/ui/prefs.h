// Persisted user preferences. Portable by design: the JSON file lives
// next to the executable (USB-stick deployment must not touch the
// registry / AppData).
#pragma once
#include "../engine/match_engine.h"
#include <QString>

namespace vj {

struct Prefs {
    QString dataDir;           // data folder (holds VJVision.db + VJVision_prefs.json + covers + standby)
    QString musicDir;          // last indexed music directory
    int deviceIdx = -1;        // capture endpoint (-1 = default loopback)
    QString standbyPath;       // optional standby logo file
    QString bgVideoPath;       // custom background media (GIF/WEBP/MP4/MOV) — used only when bgMode==1
    int bgMode = 0;            // 0 = default (built-in), 1 = custom
    float bgOverlayDepth = 0.5f; // 0 = no dim, 1 = fully black — dark overlay for legibility
    QString bgColor = "#000000"; // default background color (hex "#RRGGBB") — used when bgMode==0
    QString language = "zh";   // "zh" | "en"
    int vizMode = 0;           // 0=Mirrored Bars, 1=Radial, 2=Waterfall
    float logoSizeStandby = 1.0f;   // standby logo ratio (1.0 = full window)
    float logoSizePlaying = 0.30f;  // playing logo ratio (landscape default 0.30)
    MatchParams match;         // live recognition thresholds

    // Convenience: resolve <dataDir>/VJVision.db (empty if dataDir empty)
    QString dbPath() const;
    // Resolve <dataDir>/VJVision_prefs.json
    static QString defaultDataDir();  // <exe_dir>/data
    static QString defaultPrefsPath(); // <exe_dir>/VJVision_prefs.json (legacy fallback)

    static Prefs load();       // missing/corrupt file → built-in defaults
    void save() const;
};

} // namespace vj
