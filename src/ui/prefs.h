// Persisted user preferences. Data location adapts to how the app runs:
//   • Portable copy (writable exe folder) → <exe_dir>/data/
//   • Installed under Program Files      → ~/Documents/VJVision_data/
// Resolved at runtime; never stored in the JSON.
#pragma once
#include "../engine/i_match_engine.h"
#include <QString>

namespace vj {

struct Prefs {
    QString dataDir;           // data folder (VJVision.db + prefs JSON + covers + standby)
    QString musicDir;          // last indexed music directory
    int deviceIdx = -1;        // capture endpoint (-1 = default loopback); legacy/transient
    QString deviceId;          // WASAPI endpoint id — stable across reboots/reordering
    QString standbyPath;       // optional standby logo file
    QString bgVideoPath;       // custom background media (GIF/WEBP/MP4/MOV) — used only when bgMode==1
    int bgMode = 0;            // 0 = default (built-in), 1 = custom
    // Rhythm texture is an INDEPENDENT overlay: works on top of any bg
    // source (default/custom/future). -1 = off, 0 = pulse, 1 = breath, 2 = horizon
    int fxTexture = -1;
    int performanceMode = 0;   // 0 = auto, 1 = high, 2 = mid, 3 = low — fx perf scaling
    int prefsVersion = 3;      // migration marker (v3: pulse floor 0.13→0.10)
    float bgOverlayDepth = 0.5f; // 0 = no dim, 1 = fully black — dark overlay for legibility
    QString bgColor = "#000000"; // default background color (hex "#RRGGBB") — used when bgMode==0
    QString language = "zh";   // "zh" | "en"
    int vizMode = 0;           // 0=Mirrored Bars, 1=Radial, 2=Waterfall
    float logoSizeStandby = 1.0f;   // standby logo ratio (1.0 = full window)
    float logoSizePlaying = 0.30f;  // playing logo ratio (landscape default 0.30)
    MatchParams match;         // 引擎参数（产品 UI 不暴露；兜底/二次开发接口）

    // Convenience: resolve <dataDir>/VJVision.db
    QString dbPath() const;
    // Runtime data folder: portable <exe_dir>/data OR installed ~/Documents/VJVision_data
    static QString defaultDataDir();
    static QString defaultPrefsPath(); // <dataDir>/VJVision_prefs.json

    static Prefs load();       // missing/corrupt file → built-in defaults
    void save() const;
};

} // namespace vj
