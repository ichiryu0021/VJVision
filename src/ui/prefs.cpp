#include "prefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace vj {

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
    if (dataDir.isEmpty()) return QString();
    QDir dir(dataDir);
    return dir.filePath(QStringLiteral("VJVision.db"));
}

Prefs Prefs::load() {
    Prefs p;
    // 1. New path: <exe_dir>/data/VJVision_prefs.json
    QString prefsFile = defaultPrefsPath();
    if (!QFile::exists(prefsFile)) {
        // 2. Legacy fallback: <exe_dir>/VJVision_prefs.json
        QDir legacy(QCoreApplication::applicationDirPath());
        QString legacyFile = legacy.filePath(QStringLiteral("VJVision_prefs.json"));
        if (QFile::exists(legacyFile)) {
            // Migrate: read legacy, save to new path
            QFile lf(legacyFile);
            if (lf.open(QIODevice::ReadOnly)) {
                const auto doc = QJsonDocument::fromJson(lf.readAll());
                if (doc.isObject()) {
                    const auto o = doc.object();
                    QString legacyDbPath = o.value("dbPath").toString();
                    p.musicDir = o.value("musicDir").toString();
                    p.deviceIdx = (int)o.value("deviceIdx").toDouble(p.deviceIdx);
                    if (o.contains("language")) p.language = o.value("language").toString();
                    const auto m = o.value("match").toObject();
                    p.match.noiseFloor = (float)m.value("noiseFloor").toDouble(p.match.noiseFloor);
                    p.match.firstTrackAccept = (float)m.value("firstTrackAccept").toDouble(p.match.firstTrackAccept);
                    p.match.switchAccept = (float)m.value("switchAccept").toDouble(p.match.switchAccept);
                    p.match.confirmFrames = (int)m.value("confirmFrames").toDouble(p.match.confirmFrames);
                    if (!legacyDbPath.isEmpty()) {
                        p.dataDir = QFileInfo(legacyDbPath).absolutePath();
                    }
                }
            }
        }
    } else {
        QFile f(prefsFile);
        if (f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                const auto o = doc.object();
                p.dataDir = o.value("dataDir").toString(p.dataDir);
                p.musicDir = o.value("musicDir").toString(p.musicDir);
                p.deviceIdx = (int)o.value("deviceIdx").toDouble(p.deviceIdx);
                p.standbyPath = o.value("standbyPath").toString();
                p.bgVideoPath = o.value("bgVideoPath").toString();
                p.bgMode = (int)o.value("bgMode").toDouble(0);
                p.bgOverlayDepth = (float)o.value("bgOverlayDepth").toDouble(p.bgOverlayDepth);
                p.bgColor = o.value("bgColor").toString(p.bgColor);
                if (o.contains("language")) p.language = o.value("language").toString();

                const auto m = o.value("match").toObject();
                p.match.noiseFloor = (float)m.value("noiseFloor").toDouble(p.match.noiseFloor);
                p.match.firstTrackAccept = (float)m.value("firstTrackAccept").toDouble(p.match.firstTrackAccept);
                p.match.switchAccept = (float)m.value("switchAccept").toDouble(p.match.switchAccept);
                p.match.confirmFrames = (int)m.value("confirmFrames").toDouble(p.match.confirmFrames);
            }
        }
    }

    // Ensure dataDir is set
    if (p.dataDir.isEmpty()) {
        p.dataDir = defaultDataDir();
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
    o["dataDir"] = dataDir;
    o["musicDir"] = musicDir;
    o["deviceIdx"] = deviceIdx;
    o["standbyPath"] = standbyPath;
    o["bgVideoPath"] = bgVideoPath;
    o["bgMode"] = bgMode;
    o["bgOverlayDepth"] = bgOverlayDepth;
    o["bgColor"] = bgColor;
    o["language"] = language;
    o["match"] = m;

    QString path = defaultPrefsPath();
    // Ensure dir exists
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }
}

} // namespace vj
