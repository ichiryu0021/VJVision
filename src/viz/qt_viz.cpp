#include "qt_viz.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QScreen>
#include <QFile>
#include <QImage>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <cstdio>

namespace vj {

// Convert a QVariantList of QColor into a flat [r0,g0,b0, r1,g1,b1, ...]
// int list that QML Canvas can consume directly.
static QVariantList toFlatRgb(const QVariantList& colors) {
    QVariantList flat;
    flat.reserve(9);
    for (int i = 0; i < colors.size() && i < 3; ++i) {
        QColor c = colors[i].value<QColor>();
        flat << c.red() << c.green() << c.blue();
    }
    // Pad to 9 entries if fewer than 3 colors
    while (flat.size() < 9) {
        flat << 80 << 80 << 120; // fallback dark blue
    }
    return flat;
}

// Sample top-3 dominant colors from an image by quantizing RGB into
// 64 buckets (4 steps per channel) and picking the largest three.
// Includes near-white pixels so bright art gets a light ripple color.
static std::vector<QColor> sampleCoverColors(const QString& localPath) {
    std::vector<QColor> result;
    QImage img(localPath);
    if (img.isNull()) {
        printf("[viz] sampleCoverColors: FAILED to load '%s'\n", localPath.toUtf8().constData());
        return result;
    }
    printf("[viz] sampleCoverColors: loaded %dx%d\n", img.width(), img.height());
    img = img.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);

    // 4×4×4 = 64 buckets, each holds sum RGB + count
    struct Bucket { int64_t r=0,g=0,b=0; int count=0; };
    Bucket buckets[64];
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            QColor c = img.pixelColor(x, y);
            // Skip only near-black shadows (not near-white — bright art
            // often has large white areas we want as a ripple accent)
            int maxCh = qMax(c.red(), qMax(c.green(), c.blue()));
            if (maxCh < 20) continue;
            // Quantize: 0-63→0, 64-127→1, 128-191→2, 192-255→3
            int qr = c.red()   >> 6;
            int qg = c.green() >> 6;
            int qb = c.blue()  >> 6;
            int idx = (qr << 4) | (qg << 2) | qb;
            buckets[idx].r += c.red();
            buckets[idx].g += c.green();
            buckets[idx].b += c.blue();
            buckets[idx].count++;
        }
    }
    // Find top-3 buckets by pixel count
    int top[3] = {-1, -1, -1};
    int topCount[3] = {0, 0, 0};
    for (int i = 0; i < 64; ++i) {
        if (buckets[i].count <= 0) continue;
        if (buckets[i].count > topCount[0]) {
            top[2] = top[1]; topCount[2] = topCount[1];
            top[1] = top[0]; topCount[1] = topCount[0];
            top[0] = i;      topCount[0] = buckets[i].count;
        } else if (buckets[i].count > topCount[1]) {
            top[2] = top[1]; topCount[2] = topCount[1];
            top[1] = i;      topCount[1] = buckets[i].count;
        } else if (buckets[i].count > topCount[2]) {
            top[2] = i;      topCount[2] = buckets[i].count;
        }
    }
    for (int k = 0; k < 3 && top[k] >= 0; ++k) {
        Bucket& b = buckets[top[k]];
        result.emplace_back((int)(b.r / b.count), (int)(b.g / b.count), (int)(b.b / b.count));
    }
    return result;
}

QtVizSink::QtVizSink(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QVariantList>("QVariantList");

    // Worker-thread signals marshal onto the GUI thread (auto connection →
    // queued, since the emitters live on the capture/match threads).
    connect(this, &QtVizSink::sigTrack, this,
            [this](bool valid, QString title, QString artist, QString album,
                   QString coverPath, bool tentative, float confidence,
                   QVariantList colors) {
        hasTrack_ = valid;
        title_ = std::move(title);
        artist_ = std::move(artist);
        album_ = std::move(album);
        coverPath_ = coverPath.isEmpty()
                         ? QString()
                         : QUrl::fromLocalFile(coverPath).toString();
        tentative_ = tentative;
        confidence_ = confidence;
        if (!colors.isEmpty()) {
            trackColors_ = colors;
            trackColor_ = trackColors_.first().value<QColor>();
        }

        // Decide which colors are effective right now
        QVariantList target;
        if (valid && !coverPath.isEmpty() && QFile::exists(coverPath)) {
            // Playing a track with a real cover → use cover colors
            target = toFlatRgb(trackColors_);
        } else if (!standbyColors_.isEmpty()) {
            // No cover (standby or no-cover track) → use logo/standby colors
            target = toFlatRgb(standbyColors_);
        } else {
            // No colors at all → fallback dark blue
            target << 80 << 80 << 120 << 80 << 80 << 120 << 80 << 80 << 120;
        }
        if (target != effectiveColors_) {
            effectiveColors_ = target;
            emit effectiveColorsChanged();
        }

        emit trackChanged();
    }, Qt::QueuedConnection);

    connect(this, &QtVizSink::sigSpectrum, this,
            [this](QVariantList bins, float peak) {
        bins_ = std::move(bins);
        peak_ = peak;
        emit binsChanged();
    }, Qt::QueuedConnection);

    connect(this, &QtVizSink::sigStatus, this,
            [this](QString statusText) {
        statusText_ = std::move(statusText);
        emit statusChanged();
    }, Qt::QueuedConnection);
}

QtVizSink::~QtVizSink() {
    delete engine_;
}

bool QtVizSink::load(int screenIndex) {
    // Route QML console.log/debug onto stderr — the default Windows handler
    // only sends them to OutputDebugString, which a redirected log misses.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx,
                              const QString& msg) {
        const char* tag = "qml-log";
        if (type == QtWarningMsg || type == QtCriticalMsg) tag = "qml";
        fprintf(stderr, "[%s] %s\n", tag, msg.toUtf8().constData());
        (void)ctx;
    });

    engine_ = new QQmlApplicationEngine(this);
    QObject::connect(engine_, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError>& warnings) {
        for (const auto& e : warnings)
            fprintf(stderr, "[qml] %s\n", e.toString().toUtf8().constData());
    });
    engine_->rootContext()->setContextProperty("viz", this);

    const QUrl qmlUrl("qrc:/viz/qml/viz.qml");
    if (!QFile::exists(":/viz/qml/viz.qml")) {
        fprintf(stderr, "[viz] QML resource missing in qrc — check viz.qrc prefix\n");
    }
    engine_->load(qmlUrl);
    if (engine_->rootObjects().isEmpty()) {
        fprintf(stderr, "[viz] failed to load QML (see [qml] lines above)\n");
        return false;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine_->rootObjects().first());
    if (!window) {
        fprintf(stderr, "[viz] root object is not a Window\n");
        return false;
    }
    // Start windowed (small window) so the user can drag it to any monitor
    // and press F to fullscreen there. Windowed mode also lets the user
    // resize to arbitrary aspect ratios for composition testing.
    const auto screens = QGuiApplication::screens();
    QScreen* targetScreen = QGuiApplication::primaryScreen();
    if (screenIndex >= 0 && screenIndex < screens.size()) {
        targetScreen = screens[screenIndex];
        window->setScreen(targetScreen);
    }
    window->resize(960, 540);
    const QRect geo = targetScreen->availableGeometry();
    window->setPosition(geo.center().x() - window->width() / 2,
                        geo.center().y() - window->height() / 2);
    window->show();
    printf("[viz] windowed on screen %d (%s)\n",
           screenIndex, window->screen() ? window->screen()->name().toUtf8().constData()
                                         : "default");
    return true;
}

void QtVizSink::requestClose() { emit closeRequested(); }

void QtVizSink::onTrack(const TrackEvent& t) {
    if (!t.valid) {
        // hasTrack=false so QML contentProgress Behavior fades out,
        // but keep the previous title/artist/album/coverPath so the
        // text stays on screen *during* the fade (no "Unknown Artist" flash).
        // Next valid=true TrackEvent will overwrite all fields.
        hasTrack_ = false;
        emit trackChanged();
        return;
    }
    QString coverPath = QString::fromUtf8(t.coverPath.c_str());
    printf("[viz] onTrack: valid=%d coverPath='%s'\n", t.valid, coverPath.toUtf8().constData());
    auto colors = sampleCoverColors(coverPath);
    printf("[viz] cover colors (%zu):", colors.size());
    for (size_t i = 0; i < colors.size(); ++i)
        printf("  [%zu] rgb(%d,%d,%d)", i, colors[i].red(), colors[i].green(), colors[i].blue());
    printf("\n");
    QVariantList qv;
    if (colors.size() >= 2) {
        // Layer 0: top dominant  (main color)
        qv << colors[0];
        // Layer 1: adjacent hue (+30° from c0) — no blend with c1, keep distinct
        QColor c0 = colors[0];
        int h0, s0, l0, a0;
        c0.getHsl(&h0, &s0, &l0, &a0);
        int hShift = (h0 + 30 + 360) % 360;
        QColor adjacent;
        adjacent.setHsl(hShift, s0, l0);
        qv << adjacent;
        // Layer 2: second dominant (unchanged)
        qv << colors[1];
    } else if (colors.size() == 1) {
        qv << colors[0] << colors[0] << colors[0];
    }
    emit sigTrack(
        true,
        QString::fromUtf8(t.title.c_str()),
        QString::fromUtf8(t.artist.c_str()),
        QString::fromUtf8(t.album.c_str()),
        coverPath,
        t.tentative, t.confidence,
        qv);
}

void QtVizSink::onSpectrum(const float* bins, int count, float peak) {
    QVariantList list;
    list.reserve(count);
    for (int i = 0; i < count; ++i) list.append((double)bins[i]);
    emit sigSpectrum(std::move(list), peak);
}

void QtVizSink::onStatus(VizStatus status) {
    const char* s = "standby";
    switch (status) {
        case VizStatus::Listening: s = "listening"; break;
        case VizStatus::Matching:  s = "matched";   break;
        case VizStatus::Mixing:    s = "mixing";    break;
        case VizStatus::Standby:   s = "standby";   break;
    }
    emit sigStatus(QString::fromUtf8(s));
}

} // namespace vj
