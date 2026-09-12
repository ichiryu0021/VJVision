#include "prefs.h"

#include <cstdio>
#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace vj {

namespace {

// True when `path` lives inside any of the per-machine Program Files roots.
// Compared case-insensitively on clean absolute paths with a boundary check
// so "C:\Program Files2\..." cannot pass as "C:\Program Files\...".
bool isUnderProgramFiles(const QString& path) {
    const QString base = QDir::cleanPath(path);
    const char* vars[] = {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"};
    for (const char* v : vars) {
        const QString pf = QDir::cleanPath(
            QString::fromLocal8Bit(qgetenv(v)));
        if (pf.isEmpty() || base.size() < pf.size()) continue;
        if (!base.startsWith(pf, Qt::CaseInsensitive)) continue;
        if (base.size() == pf.size() ||
            base.at(pf.size()) == QLatin1Char('/') ||
            base.at(pf.size()) == QLatin1Char('\\'))
            return true;
    }
    return false;
}

// One-shot rescue for builds stranded next to the exe by older versions:
// an elevated first launch (e.g. Inno's post-install "launch" checkbox) used
// to make the writability probe succeed inside Program Files, so prefs/DB
// were written to <app>\data. Copy (never move — the source may be in a
// read-only location) that tree into the Documents data folder once.
void migrateStrandedExeData(const QString& exeDir, const QString& targetData) {
    const QString targetPrefs =
        QDir(targetData).filePath(QStringLiteral("VJVision_prefs.json"));
    if (QFileInfo::exists(targetPrefs)) return;  // installed mode already used

    const QString legacy = QDir(exeDir).filePath(QStringLiteral("data"));
    if (!QFileInfo::exists(QDir(legacy).filePath(
            QStringLiteral("VJVision_prefs.json"))))
        return;  // nothing stranded

    std::function<bool(const QString&, const QString&)> copyTree =
        [&](const QString& src, const QString& dst) -> bool {
        QDir().mkpath(dst);
        const auto entries = QDir(src).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& e : entries) {
            const QString to = QDir(dst).filePath(e.fileName());
            if (e.isDir()) {
                if (!copyTree(e.absoluteFilePath(), to)) return false;
            } else if (!QFileInfo::exists(to)) {
                if (!QFile::copy(e.absoluteFilePath(), to)) return false;
            }
        }
        return true;
    };

    if (copyTree(legacy, targetData)) {
        fprintf(stderr, "[prefs] migrated stranded data from %s to %s\n",
                legacy.toLocal8Bit().constData(),
                targetData.toLocal8Bit().constData());
    }
}

}  // namespace

// Single source of truth for ALL runtime data — prefs + DB + covers.
//
// Mode detection MUST be independent of the privilege level of the current
// process (an elevated writability probe inside Program Files used to flip
// the mode and strand settings in <app>\data):
//   • Installed edition → ~/Documents/VJVision_data/. Detected by an
//     "installed.flag" marker the installer writes next to the exe, with a
//     Program Files path check as a defensive fallback.
//   • Portable (ZIP / USB / normal folder) → <exe_dir>/data/, so the app
//     and data travel together. A read-only portable medium (probe fails)
//     still falls back to Documents.
QString Prefs::defaultDataDir() {
    const QString exeDir = QCoreApplication::applicationDirPath();

    const bool markerInstalled = QFileInfo::exists(
        QDir(exeDir).filePath(QStringLiteral("installed.flag")));
    const bool installedMode =
        markerInstalled || isUnderProgramFiles(exeDir);

    QString data;
    if (installedMode) {
        const QString docs =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        data = QDir(docs).filePath(QStringLiteral("VJVision_data"));
    } else {
        // Write probe next to the exe — only meaningful for portable copies
        // (marker / Program Files already classified above).
        bool exeWritable = false;
        {
            const QString probe =
                QDir(exeDir).filePath(QStringLiteral(".vj_write_probe.tmp"));
            QFile f(probe);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.close();
                f.remove();
                exeWritable = true;
            }
        }
        if (exeWritable) {
            data = QDir(exeDir).filePath(QStringLiteral("data"));
        } else {
            const QString docs =
                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            data = QDir(docs).filePath(QStringLiteral("VJVision_data"));
        }
    }

    if (!QDir(data).exists())
        QDir().mkpath(data);

    if (installedMode)
        migrateStrandedExeData(exeDir, data);

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
            p.deviceId = o.value("deviceId").toString();
            p.standbyPath = o.value("standbyPath").toString();
            p.bgVideoPath = o.value("bgVideoPath").toString();
            p.bgMode = (int)o.value("bgMode").toDouble(0);
            // v2.1+: fxTexture is an independent overlay (-1 = off).
            // Old files had it as a bgMode==2-only selector defaulting to 0.
            const int ver = (int)o.value("prefsVersion").toDouble(0);
            p.fxTexture = (int)o.value("fxTexture").toDouble(ver >= 2 ? -1 : 0);
            // v1 → v2 migration: bgMode 2 ("rhythm texture" as a bg source)
            // no longer exists. Users who used it keep the texture playing
            // over the default bg; everyone else gets the new default (off),
            // since their stored fxTexture was never active before.
            if (ver < 2) {
                if (p.bgMode == 2) {
                    p.bgMode = 0;
                } else {
                    p.fxTexture = -1;
                }
            }
            if (p.bgMode != 0 && p.bgMode != 1) p.bgMode = 0;
            if (p.fxTexture < -1 || p.fxTexture > 2) p.fxTexture = -1;
            p.prefsVersion = 3;
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

            // 电量产品不暴露识别阈值；开源置信引擎使用编译期默认。
            // 旧版本写入的 "match" 键存在时忽略。
        }
    }
    return p;
}

void Prefs::save() const {
    QJsonObject o;
    // dataDir intentionally NOT saved — resolved at runtime (portable exe/data
    // vs installed Documents/VJVision_data) via defaultDataDir().
    o["musicDir"] = musicDir;
    o["deviceIdx"] = deviceIdx;
    o["deviceId"] = deviceId;
    o["standbyPath"] = standbyPath;
    o["bgVideoPath"] = bgVideoPath;
    o["bgMode"] = bgMode;
    o["fxTexture"] = fxTexture;
    o["prefsVersion"] = prefsVersion;
    o["performanceMode"] = performanceMode;
    o["bgOverlayDepth"] = bgOverlayDepth;
    o["bgColor"] = bgColor;
    o["language"] = language;
    o["vizMode"] = vizMode;
    o["logoSizeStandby"] = logoSizeStandby;
    o["logoSizePlaying"] = logoSizePlaying;

    QString path = defaultPrefsPath();
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }
}

} // namespace vj
