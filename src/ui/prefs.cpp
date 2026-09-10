#include "prefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace vj {

// Single source of truth for ALL runtime data — prefs + DB + covers.
// Everything lives under <exe_dir>/data/. No legacy paths, no migration.
QString Prefs::defaultDataDir() {
    QDir dir(QCoreApplication::applicationDirPath());
    QString data = dir.filePath(QStringLiteral("data"));
    if (!QDir(data).exists()) {
        QDir().mkpath(data);
    }
    return data;
}

QString Prefs::defaultPrefsPath() {
    QDir dir(defaultDataDir());
    return dir.filePath(QStringLiteral("VJVision_prefs.json"));
}

QString Prefs::dbPath() const {
    QDir dir(defaultDataDir());   // always <exe_dir>/data, never stale dataDir
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
    // dataDir intentionally NOT saved — always resolved to <exe_dir>/data at runtime
    o["musicDir"] = musicDir;
    o["deviceIdx"] = deviceIdx;
    o["standbyPath"] = standbyPath;
    o["bgVideoPath"] = bgVideoPath;
    o["bgMode"] = bgMode;
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
