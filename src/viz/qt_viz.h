// In-process Qt Quick renderer: a VizSink implementation that exposes
// state to QML via context properties. Worker threads (capture/match) may
// call the VizSink methods directly; signals cross to the GUI thread
// through Qt's queued connections.
#pragma once
#include "viz_events.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QColor>
#include <QImage>
#include <QUrl>
#include <QFile>

class QQmlApplicationEngine;
class QQuickWindow;

namespace vj {

class QtVizSink : public QObject, public VizSink {
    Q_OBJECT
    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY trackChanged)
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY trackChanged)
    Q_PROPERTY(QString album READ album NOTIFY trackChanged)
    Q_PROPERTY(bool tentative READ tentative NOTIFY trackChanged)
    Q_PROPERTY(float confidence READ confidence NOTIFY trackChanged)
    Q_PROPERTY(QString coverPath READ coverPath NOTIFY trackChanged)
    Q_PROPERTY(QVariantList bins READ bins NOTIFY binsChanged)
    Q_PROPERTY(float peak READ peak NOTIFY binsChanged)
    Q_PROPERTY(float beat READ beat NOTIFY binsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString standbyPath READ standbyPath NOTIFY standbyPathChanged)
    Q_PROPERTY(QString bgVideoPath READ bgVideoPath NOTIFY bgVideoPathChanged)
    Q_PROPERTY(float bgOverlayDepth READ bgOverlayDepth NOTIFY bgOverlayDepthChanged)
    Q_PROPERTY(QString bgColor READ bgColor NOTIFY bgColorChanged)
    Q_PROPERTY(QColor trackColor READ trackColor NOTIFY trackChanged)
    Q_PROPERTY(QVariantList trackColors READ trackColors NOTIFY trackChanged)
    Q_PROPERTY(QVariantList standbyColors READ standbyColors NOTIFY standbyColorsChanged)
    // Flat list [r0,g0,b0, r1,g1,b1, r2,g2,b2] — integers 0-255.
    // QML Canvas binds this and does its own lerp for fade transitions.
    Q_PROPERTY(QVariantList effectiveColors READ effectiveColors NOTIFY effectiveColorsChanged)
    Q_PROPERTY(int vizMode READ vizMode NOTIFY vizModeChanged)
    Q_PROPERTY(int bgMode READ bgMode NOTIFY bgModeChanged)
    Q_PROPERTY(int fxTexture READ fxTexture NOTIFY fxTextureChanged)
    Q_PROPERTY(int performanceMode READ performanceMode NOTIFY performanceModeChanged)
    Q_PROPERTY(float logoSizeStandby READ logoSizeStandby NOTIFY logoSizeStandbyChanged)
    Q_PROPERTY(float logoSizePlaying READ logoSizePlaying NOTIFY logoSizePlayingChanged)

public:
    explicit QtVizSink(QObject* parent = nullptr);
    ~QtVizSink() override;

    // Create the QML window on the given screen index (-1 = primary).
    // Returns false on QML load errors.
    bool load(int screenIndex);

    // Called from QML when the user presses Escape on the fullscreen
    // window. The owner (VizController) tears the session down; the
    // panel itself stays open.
    Q_INVOKABLE void requestClose();

    // VizSink — safe to call from any thread.
    void onTrack(const TrackEvent& t) override;
    void onSpectrum(const float* bins, int count, float peak,
                    float beat = 0.f) override;
    void onStatus(VizStatus status) override;

    bool hasTrack() const { return hasTrack_; }
    QString title() const { return title_; }
    QString artist() const { return artist_; }
    QString album() const { return album_; }
    bool tentative() const { return tentative_; }
    float confidence() const { return confidence_; }
    QString coverPath() const { return coverPath_; }
    QVariantList bins() const { return bins_; }
    float peak() const { return peak_; }
    float beat() const { return beat_; }
    QString statusText() const { return statusText_; }
    QString standbyPath() const { return standbyPath_; }
    Q_INVOKABLE void setStandbyPath(const QString& p) {
        // Convert "file:///C:/path.png" → "C:/path.png"
        QString localPath = p;
        if (localPath.startsWith("file:///"))
            localPath = localPath.mid(8);  // strip "file:///"
        printf("[viz] setStandbyPath: p='%s' localPath='%s'\n",
               p.toUtf8().constData(), localPath.toUtf8().constData());
        if (standbyPath_ != p) { standbyPath_ = p; emit standbyPathChanged(); }

        // Sample colors from the standby image
        if (!localPath.isEmpty()) {
            QImage img(localPath);
            printf("[viz] setStandbyPath: img.isNull()=%d size=%dx%d\n",
                   img.isNull(), img.width(), img.height());
            if (!img.isNull()) {
                img = img.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                struct Bucket { int64_t r=0,g=0,b=0; int count=0; };
                Bucket buckets[64];
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        QColor c = img.pixelColor(x, y);
                        int maxCh = qMax(c.red(), qMax(c.green(), c.blue()));
                        if (maxCh < 20) continue;
                        int qr = c.red() >> 6, qg = c.green() >> 6, qb = c.blue() >> 6;
                        int idx = (qr << 4) | (qg << 2) | qb;
                        buckets[idx].r += c.red(); buckets[idx].g += c.green();
                        buckets[idx].b += c.blue(); buckets[idx].count++;
                    }
                }
                int top[3]={-1,-1,-1}, topCount[3]={0,0,0};
                for (int i=0;i<64;++i) {
                    if (buckets[i].count<=0) continue;
                    if (buckets[i].count>topCount[0]) {
                        top[2]=top[1]; topCount[2]=topCount[1];
                        top[1]=top[0]; topCount[1]=topCount[0];
                        top[0]=i; topCount[0]=buckets[i].count;
                    } else if (buckets[i].count>topCount[1]) {
                        top[2]=top[1]; topCount[2]=topCount[1];
                        top[1]=i; topCount[1]=buckets[i].count;
                    } else if (buckets[i].count>topCount[2]) {
                        top[2]=i; topCount[2]=buckets[i].count;
                    }
                }
                QVariantList qv;
                for (int k=0;k<3 && top[k]>=0;++k) {
                    Bucket& b=buckets[top[k]];
                    qv << QColor((int)(b.r/b.count),(int)(b.g/b.count),(int)(b.b/b.count));
                }
                if (!qv.isEmpty()) {
                    standbyColors_ = qv;
                    emit standbyColorsChanged();
                    if (!hasTrack_) {
                        effectiveColors_.clear();
                        for (int i=0;i<qv.size()&&i<3;++i) {
                            QColor c = qv[i].value<QColor>();
                            effectiveColors_ << c.red() << c.green() << c.blue();
                        }
                        while (effectiveColors_.size()<9)
                            effectiveColors_ << 80 << 80 << 120;
                        emit effectiveColorsChanged();
                    }
                }
            }
        }
    }
    QString bgVideoPath() const { return bgVideoPath_; }
    Q_INVOKABLE void setBgVideoPath(const QString& p) {
        fprintf(stderr, "[QtVizSink] setBgVideoPath: '%s' → '%s'\n",
                bgVideoPath_.toUtf8().constData(), p.toUtf8().constData());
        if (bgVideoPath_ != p) { bgVideoPath_ = p; emit bgVideoPathChanged(); }
    }
    float bgOverlayDepth() const { return bgOverlayDepth_; }
    Q_INVOKABLE void setBgOverlayDepth(float v) {
        v = qBound(0.f, v, 1.f);
        fprintf(stderr, "[QtVizSink] setBgOverlayDepth: %f\n", v);
        if (!qFuzzyCompare(bgOverlayDepth_, v)) { bgOverlayDepth_ = v; emit bgOverlayDepthChanged(); }
        else emit bgOverlayDepthChanged();  // always emit so QML gets initial push
    }
    QString bgColor() const { return bgColor_; }
    Q_INVOKABLE void setBgColor(const QString& c) {
        fprintf(stderr, "[QtVizSink] setBgColor: '%s'\n", c.toUtf8().constData());
        if (bgColor_ != c) { bgColor_ = c; emit bgColorChanged(); }
        else emit bgColorChanged();  // always emit so QML gets initial push
    }
    QColor trackColor() const { return trackColor_; }
    QVariantList trackColors() const { return trackColors_; }
    QVariantList standbyColors() const { return standbyColors_; }
    QVariantList effectiveColors() const { return effectiveColors_; }

    int vizMode() const { return vizMode_; }
    Q_INVOKABLE void setVizMode(int v) {
        v = qBound(0, v, 2);
        fprintf(stderr, "[QtVizSink] setVizMode: %d → %d\n", vizMode_, v);
        if (vizMode_ != v) { vizMode_ = v; emit vizModeChanged(); }
        else emit vizModeChanged();  // always emit so QML gets initial push
    }

    int bgMode() const { return bgMode_; }
    Q_INVOKABLE void setBgMode(int v) {
        v = qBound(0, v, 1);   // 0 = default, 1 = custom
        if (bgMode_ != v) { bgMode_ = v; emit bgModeChanged(); }
        else emit bgModeChanged();
    }
    int fxTexture() const { return fxTexture_; }
    Q_INVOKABLE void setFxTexture(int v) {
        v = qBound(-1, v, 2);  // -1 = off, 0 = pulse, 1 = breath, 2 = horizon
        if (fxTexture_ != v) { fxTexture_ = v; emit fxTextureChanged(); }
        else emit fxTextureChanged();
    }
    int performanceMode() const { return performanceMode_; }
    Q_INVOKABLE void setPerformanceMode(int v) {
        v = qBound(0, v, 3);
        if (performanceMode_ != v) { performanceMode_ = v; emit performanceModeChanged(); }
        else emit performanceModeChanged();
    }

    float logoSizeStandby() const { return logoSizeStandby_; }
    Q_INVOKABLE void setLogoSizeStandby(float v) {
        v = qBound(0.3f, v, 1.5f);
        if (!qFuzzyCompare(logoSizeStandby_, v)) { logoSizeStandby_ = v; emit logoSizeStandbyChanged(); }
        else emit logoSizeStandbyChanged();
    }
    float logoSizePlaying() const { return logoSizePlaying_; }
    Q_INVOKABLE void setLogoSizePlaying(float v) {
        v = qBound(0.05f, v, 0.8f);
        if (!qFuzzyCompare(logoSizePlaying_, v)) { logoSizePlaying_ = v; emit logoSizePlayingChanged(); }
        else emit logoSizePlayingChanged();
    }

signals:
    void trackChanged();
    void binsChanged();
    void statusChanged();
    void standbyPathChanged();
    void bgVideoPathChanged();
    void closeRequested();
    void standbyColorsChanged();
    void effectiveColorsChanged();
    void bgOverlayDepthChanged();
    void bgColorChanged();
    void vizModeChanged();
    void bgModeChanged();
    void fxTextureChanged();
    void performanceModeChanged();
    void logoSizeStandbyChanged();
    void logoSizePlayingChanged();

    // Cross-thread transport (emitted from worker threads).
    void sigTrack(bool valid, QString title, QString artist, QString album,
                  QString coverPath, bool tentative, float confidence,
                  QVariantList colors);
    void sigSpectrum(QVariantList bins, float peak, float beat);
    void sigStatus(QString statusText);

    // GPU TDR / device-lost recovery lifecycle.
    void gpuRecovered(int attempt);
    void gpuRecoveryFailed();

private:
    bool recreateEngine();               // core: delete + reload + re-bind + re-emit
    void onSceneGraphError(int error, const QString& msg);
    void scheduleRecovery();

    QQmlApplicationEngine* engine_ = nullptr;
    QQuickWindow* vizWindow_ = nullptr;   // cached after load()

    bool hasTrack_ = false;
    QString title_;
    QString artist_;
    QString album_;
    QString coverPath_;
    bool tentative_ = false;
    float confidence_ = 0.f;
    QVariantList bins_;
    float peak_ = 0.f;
    float beat_ = 0.f;
    QString statusText_ = QStringLiteral("standby");
    QString standbyPath_;
    QString bgVideoPath_;
    float bgOverlayDepth_ = 0.5f;
    QString bgColor_ = "#000000";
    QColor trackColor_{48, 80, 120};
    QVariantList trackColors_{QColor(48,80,120), QColor(80,48,120), QColor(120,48,80)};
    QVariantList standbyColors_;   // empty → will be filled when standbyPath is set
    QVariantList effectiveColors_; // flat [r0,g0,b0, r1,g1,b1, r2,g2,b2]
    int vizMode_ = 0;
    int bgMode_ = 0;
    int fxTexture_ = -1;   // -1 = off (independent overlay since v2.1)
    int performanceMode_ = 0;
    float logoSizeStandby_ = 1.0f;
    float logoSizePlaying_ = 0.30f;

    // --- TDR recovery state ---
    int screenIndex_ = -1;               // which screen we launched on
    qint64 lastRecoveryMs_ = 0;          // throttle: min 5s between attempts
    int recoveryAttempts_ = 0;           // throttle: max 5 consecutive
    static constexpr int kMinRecoveryIntervalMs = 5000;
    static constexpr int kMaxRecoveryAttempts = 5;
    static constexpr int kRecoveryDelayMs = 500;  // wait for GPU to settle
};

} // namespace vj
