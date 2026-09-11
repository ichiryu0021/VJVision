#include "prefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace vj {

// Single source of truth for ALL runtime data — prefs + DB + covers.
//
// Location is picked at runtime by probing whether the exe folder is writable:
//   • Portable (ZIP on a normal folder / USB stick) → <exe_dir>/data/
//     so the whole app + data travel together.
//   • Installed under a read-only location (C:\Program Files) →
//     ~/Documents/VJVision_data/, where every user can find it easily.
QString Prefs::defaultDataDir() {
    const QString exeDir = QCoreApplication::applicationDirPath();

    // Write probe next to the exe. Program Files is read-only for a
    // non-elevated process, so this fails for the installed edition and
    // succeeds for a portable copy.
    bool exeWritable = false;
    {
        const QString probe = QDir(exeDir).filePath(QStringLiteral(".vj_write_probe.tmp"));
        QFile f(probe);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.close();
            f.remove();
            exeWritable = true;
        }
    }

    QString data;
    if (exeWritable) {
        // Portable mode: keep data beside the exe.
        data = QDir(exeDir).filePath(QStringLiteral("data"));
    } else {
        // Installed mode: user data goes under Documents (easy to find).
        const QString docs =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        data = QDir(docs).filePath(QStringLiteral("VJVision_data"));
    }

    if (!QDir(data).exists())
        QDir().mkpath(data);
    return data;
}

QString Prefs::defaultPrefsPath() {
    QDir dir(defaultDataDir());
    return dir.filePath(QStringLiteral("VJVision_prefs.json"));
}

QString Prefs::dbPath() const {
    QDir dir(defaultDataDir());   // resolved fresh each call (portable vs installed)
    return dir.filePath(QStringLiteral("VJVision.db"));
}

Prefs Prefs::load() {
    Prefs p;
    p.dataDir = defaultDataDir();   // always set first — never trust JSON dataDir

    QFile f(defaultPrefsPath());
    if (f.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            const auto o = doc.object();
            p.musicDir = o.value("musicDir").toString();
            p.deviceIdx = (int)o.value("deviceIdx").toDouble(p.deviceIdx);
            p.standbyPath = o.value("standbyPath").toString();
            p.bgVideoPath = o.value("bgVideoPath").toString();
            p.bgMode = (int)o.value("bgMode").toDouble(0);
            p.fxTexture = (int)o.value("fxTexture").toDouble(0);
            p.performanceMode = (int)o.value("performanceMode").toDouble(0);
            p.bgOverlayDepth = (float)o.value("bgOverlayDepth").toDouble(p.bgOverlayDepth);
            p.bgColor = o.value("bgColor").toString();
            if (p.bgColor.isEmpty()) p.bgColor = "#000000";
            if (o.contains("language")) p.language = o.value("language").toString();
            p.vizMode = (int)o.value("vizMode").toDouble(0);
            p.logoSizeStandby = (float)o.value("logoSizeStandby").toDouble(p.logoSizeStandby);
            p.logoSizePlaying = (float)o.value("logoSizePlaying").toDouble(p.logoSizePlaying);
            if (p.logoSizeStandby < 0.3f || p.logoSizeStandby > 1.5f) p.logoSizeStandby = 1.0f;
            if (p.logoSizePlaying < 0.05f || p.logoSizePlaying > 0.8f) p.logoSizePlaying = 0.30f;

            const auto m = o.value("match").toObject();
            p.match.noiseFloor = (float)m.value("noiseFloor").toDouble(p.match.noiseFloor);
            p.match.firstTrackAccept = (float)m.value("firstTrackAccept").toDouble(p.match.firstTrackAccept);
            p.match.switchAccept = (float)m.value("switchAccept").toDouble(p.match.switchAccept);
            p.match.confirmFrames = (int)m.value("confirmFrames").toDouble(p.match.confirmFrames);
        }
    }
    return p;
}

void Prefs::save() const {
    QJsonObject m;
    m["noiseFloor"] = match.noiseFloor;
    m["firstTrackAccept"] = match.firstTrackAccept;
    m["switchAccept"] = match.switchAccept;
    m["confirmFrames"] = match.confirmFrames;

    QJsonObject o;
    // dataDir intentionally NOT saved — resolved at runtime (portable exe/data
    // vs installed Documents/VJVision_data) via defaultDataDir().
    o["musicDir"] = musicDir;
    o["deviceIdx"] = deviceIdx;
    o["standbyPath"] = standbyPath;
    o["bgVideoPath"] = bgVideoPath;
    o["bgMode"] = bgMode;
    o["fxTexture"] = fxTexture;
    o["performanceMode"] = performanceMode;
    o["bgOverlayDepth"] = bgOverlayDepth;
    o["bgColor"] = bgColor;
    o["language"] = language;
    o["vizMode"] = vizMode;
    o["logoSizeStandby"] = logoSizeStandby;
    o["logoSizePlaying"] = logoSizePlaying;
    o["match"] = m;

    QString path = defaultPrefsPath();
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }
}

} // namespace vj
