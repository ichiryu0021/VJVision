#include "prefs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace vj {
namespace {
QString prefsPath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("vjvcplus_prefs.json");
}
} // namespace

QString Prefs::defaultPath() { return prefsPath(); }

Prefs Prefs::load() {
    Prefs p;
    QFile f(prefsPath());
    if (!f.open(QIODevice::ReadOnly)) return p;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return p;
    const auto o = doc.object();

    p.dbPath = o.value("dbPath").toString(p.dbPath);
    p.musicDir = o.value("musicDir").toString(p.musicDir);
    p.deviceIdx = (int)o.value("deviceIdx").toDouble(p.deviceIdx);
    p.screenIdx = (int)o.value("screenIdx").toDouble(p.screenIdx);
    if (o.contains("language")) p.language = o.value("language").toString();

    const auto m = o.value("match").toObject();
    p.match.noiseFloor = (float)m.value("noiseFloor").toDouble(p.match.noiseFloor);
    p.match.firstTrackAccept =
        (float)m.value("firstTrackAccept").toDouble(p.match.firstTrackAccept);
    p.match.switchAccept =
        (float)m.value("switchAccept").toDouble(p.match.switchAccept);
    p.match.confirmFrames =
        (int)m.value("confirmFrames").toDouble(p.match.confirmFrames);
    return p;
}

void Prefs::save() const {
    QJsonObject m;
    m["noiseFloor"] = match.noiseFloor;
    m["firstTrackAccept"] = match.firstTrackAccept;
    m["switchAccept"] = match.switchAccept;
    m["confirmFrames"] = match.confirmFrames;

    QJsonObject o;
    o["dbPath"] = dbPath;
    o["musicDir"] = musicDir;
    o["deviceIdx"] = deviceIdx;
    o["screenIdx"] = screenIdx;
    o["language"] = language;
    o["match"] = m;

    QFile f(prefsPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }
}

} // namespace vj
