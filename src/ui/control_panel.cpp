#include "control_panel.h"

#include "../audio/wasapi_capture.h"
#include "../engine/indexer.h"
#include "../fp/fp_db.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace vj {

namespace {
// Minimal bilingual dictionary (zh / en). Visual style strings stay
// language-neutral (the "PEAK" indicator is always English by spec).
const char* tr2(const QString& lang, const char* key) {
    static const struct { const char* k; const char* zh; const char* en; } rows[] = {
        {"appTitle",       "VJVision 控制面板",            "VJVision Control Panel"},
        {"grpAudio",       "音频与显示",                   "Audio & Display"},
        {"device",         "音频设备",                     "Audio device"},
        {"refresh",        "刷新",                         "Refresh"},
        {"screen",         "全屏显示器",                   "Fullscreen monitor"},
        {"grpLib",         "曲库",                         "Music Library"},
        {"dbPath",         "指纹数据库",                   "Fingerprint DB"},
        {"browse",         "浏览…",                        "Browse…"},
        {"musicDir",       "音乐目录",                     "Music directory"},
        {"index",          "索引目录",                     "Index directory"},
        {"indexing",       "索引中…",                      "Indexing…"},
        {"indexRun",       "执行索引",                     "Run indexing"},
        {"indexStatus",    "索引状态",                     "Index status"},
        {"songCount",      "已索引歌曲",                   "Indexed songs"},
        {"level",          "输入电平",                     "Input level"},
        {"grpThr",         "识别阈值（即时生效）",         "Recognition thresholds (live)"},
        {"noise",          "噪声下限",                     "Noise floor"},
        {"first",          "首曲确认",                     "First-track accept"},
        {"switch",         "切歌确认",                     "Switch accept"},
        {"confirm",        "确认次数",                     "Confirm frames"},
        {"grpRun",         "运行",                         "Run"},
        {"startViz",       "启动可视化（全屏）",           "Start visualizer (fullscreen)"},
        {"stopViz",        "停止可视化",                   "Stop visualizer"},
        {"language",       "语言",                         "Language"},
        {"log",            "日志",                         "Log"},
        {"needDb",         "请先选择数据库路径",           "Pick a database path first"},
        {"needDir",        "请先选择音乐目录",             "Pick a music directory first"},
        {"indexDone",      "索引完成",                     "Index complete"},
        {"indexFailed",    "索引失败：无法打开数据库",     "Index failed: cannot open DB"},
        {"closing",        "正在关闭，等待索引线程结束…",  "Closing, waiting for indexer…"},
    };
    for (const auto& r : rows) {
        if (qstrcmp(r.k, key) == 0)
            return lang == "en" ? r.en : r.zh;
    }
    return key;
}
} // namespace

ControlPanel::ControlPanel(QWidget* parent) : QWidget(parent) {
    prefs_ = Prefs::load();
    if (prefs_.dbPath.isEmpty())
        prefs_.dbPath = QDir(QCoreApplication::applicationDirPath())
                            .filePath("VJVision.db");
    buildUi();
    loadPrefsToUi();
    retranslate();
    refreshDevices();
    refreshScreens();
    refreshSongCount();

    controller_ = std::make_unique<VizController>();
    connect(controller_.get(), &VizController::logMessage,
            this, &ControlPanel::appendLog);
    connect(controller_.get(), &VizController::sessionStopped,
            this, &ControlPanel::onSessionStopped);

    levelTimer_ = new QTimer(this);
    connect(levelTimer_, &QTimer::timeout, this, [this] {
        if (!controller_) return;
        levelBar_->setValue(qBound(0, int(controller_->peak() * 100.0), 100));
    });
    levelTimer_->start(50);

    appendLog(QStringLiteral("VJVision control panel ready."));
}

ControlPanel::~ControlPanel() = default;

QString ControlPanel::t(const char* key) const {
    return QString::fromUtf8(tr2(prefs_.language, key));
}

void ControlPanel::buildUi() {
    setMinimumWidth(720);
    auto* root = new QVBoxLayout(this);

    // Labels carry their dictionary key as objectName; retranslate()
    // walks findChildren<QLabel*> and updates them in one loop.
    auto lbl = [](const char* key) {
        auto* l = new QLabel;
        l->setObjectName(QString::fromUtf8(key));
        return l;
    };

    // --- Audio & display -------------------------------------------------
    grpAudio_ = new QGroupBox;
    auto* audioForm = new QFormLayout(grpAudio_);
    auto* devRow = new QHBoxLayout();
    deviceCombo_ = new QComboBox;
    refreshDevBtn_ = new QPushButton;
    connect(refreshDevBtn_, &QPushButton::clicked, this, [this] {
        refreshDevices();
        syncPrefs();
    });
    devRow->addWidget(deviceCombo_, 1);
    devRow->addWidget(refreshDevBtn_);
    audioForm->addRow(lbl("device"), devRow);
    screenCombo_ = new QComboBox;
    connect(screenCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { syncPrefs(); });
    audioForm->addRow(lbl("screen"), screenCombo_);
    root->addWidget(grpAudio_);

    // --- Library ---------------------------------------------------------
    grpLib_ = new QGroupBox;
    auto* libForm = new QFormLayout(grpLib_);
    auto* dbRow = new QHBoxLayout();
    dbEdit_ = new QLineEdit;
    browseDbBtn_ = new QPushButton;
    connect(browseDbBtn_, &QPushButton::clicked, this, &ControlPanel::browseDb);
    dbRow->addWidget(dbEdit_, 1);
    dbRow->addWidget(browseDbBtn_);
    libForm->addRow(lbl("dbPath"), dbRow);
    auto* dirRow = new QHBoxLayout();
    dirEdit_ = new QLineEdit;
    browseDirBtn_ = new QPushButton;
    connect(browseDirBtn_, &QPushButton::clicked, this, &ControlPanel::browseMusicDir);
    dirRow->addWidget(dirEdit_, 1);
    dirRow->addWidget(browseDirBtn_);
    libForm->addRow(lbl("musicDir"), dirRow);
    auto* idxRow = new QHBoxLayout();
    indexBtn_ = new QPushButton;
    connect(indexBtn_, &QPushButton::clicked, this, &ControlPanel::startIndex);
    indexBar_ = new QProgressBar;
    indexBar_->setRange(0, 100);
    indexBar_->setValue(0);
    idxRow->addWidget(indexBtn_);
    idxRow->addWidget(indexBar_, 1);
    libForm->addRow(lbl("indexRun"), idxRow);
    indexLabel_ = new QLabel;
    indexLabel_->setObjectName("indexStatusValue");
    songCountLabel_ = new QLabel("0");
    libForm->addRow(lbl("indexStatus"), indexLabel_);
    libForm->addRow(lbl("songCount"), songCountLabel_);
    root->addWidget(grpLib_);

    // --- Thresholds ------------------------------------------------------
    grpThr_ = new QGroupBox;
    auto* thrForm = new QFormLayout(grpThr_);
    auto makeSpin = [](double v) {
        auto* s = new QDoubleSpinBox;
        s->setRange(0.05, 0.95);
        s->setSingleStep(0.01);
        s->setDecimals(2);
        s->setValue(v);
        return s;
    };
    noiseSpin_ = makeSpin(0.13);
    firstSpin_ = makeSpin(0.25);
    switchSpin_ = makeSpin(0.30);
    confirmSpin_ = new QSpinBox;
    confirmSpin_->setRange(1, 5);
    confirmSpin_->setValue(2);
    thrForm->addRow(lbl("noise"), noiseSpin_);
    thrForm->addRow(lbl("first"), firstSpin_);
    thrForm->addRow(lbl("switch"), switchSpin_);
    thrForm->addRow(lbl("confirm"), confirmSpin_);
    for (auto* w : {noiseSpin_, firstSpin_, switchSpin_}) {
        connect(w, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this] { syncPrefs(); });
    }
    connect(confirmSpin_, qOverload<int>(&QSpinBox::valueChanged),
            this, [this] { syncPrefs(); });
    root->addWidget(grpThr_);

    // --- Run -------------------------------------------------------------
    grpRun_ = new QGroupBox;
    auto* runForm = new QFormLayout(grpRun_);
    auto* btnRow = new QHBoxLayout();
    vizBtn_ = new QPushButton;
    connect(vizBtn_, &QPushButton::clicked, this, &ControlPanel::toggleViz);
    langCombo_ = new QComboBox;
    langCombo_->addItem("中文", "zh");
    langCombo_->addItem("English", "en");
    connect(langCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
        prefs_.language = langCombo_->currentData().toString();
        syncPrefs();
        retranslate();
    });
    btnRow->addWidget(vizBtn_, 1);
    btnRow->addWidget(lbl("language"));
    btnRow->addWidget(langCombo_);
    runForm->addRow(QString(), btnRow);
    levelBar_ = new QProgressBar;
    levelBar_->setRange(0, 100);
    levelBar_->setValue(0);
    levelBar_->setFormat("PEAK %p%");
    runForm->addRow(lbl("level"), levelBar_);
    root->addWidget(grpRun_);

    // --- Log -------------------------------------------------------------
    root->addWidget(lbl("log"));
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    root->addWidget(logView_, 1);
}

void ControlPanel::retranslate() {
    setWindowTitle(t("appTitle"));
    grpAudio_->setTitle(t("grpAudio"));
    grpLib_->setTitle(t("grpLib"));
    grpThr_->setTitle(t("grpThr"));
    grpRun_->setTitle(t("grpRun"));
    refreshDevBtn_->setText(t("refresh"));
    browseDbBtn_->setText(t("browse"));
    browseDirBtn_->setText(t("browse"));
    indexBtn_->setText(indexing_.load() ? t("indexing") : t("index"));
    vizBtn_->setText(controller_ && controller_->isRunning()
                         ? t("stopViz") : t("startViz"));

    // Form labels: objectName == dictionary key. The fallback in tr2()
    // returns the key itself, so a missing translation is detectable.
    for (QLabel* l : findChildren<QLabel*>()) {
        const QByteArray key = l->objectName().toUtf8();
        if (key.isEmpty() || key == "indexStatusValue") continue;
        const char* txt = tr2(prefs_.language, key.constData());
        if (qstrcmp(txt, key.constData()) != 0)
            l->setText(QString::fromUtf8(txt));
    }
}

void ControlPanel::loadPrefsToUi() {
    dbEdit_->setText(prefs_.dbPath);
    dirEdit_->setText(prefs_.musicDir);
    noiseSpin_->setValue(prefs_.match.noiseFloor);
    firstSpin_->setValue(prefs_.match.firstTrackAccept);
    switchSpin_->setValue(prefs_.match.switchAccept);
    confirmSpin_->setValue(prefs_.match.confirmFrames);
    const int li = langCombo_->findData(prefs_.language);
    if (li >= 0) langCombo_->setCurrentIndex(li);
    // device / screen combos are populated after construction; their
    // selection happens inside refreshDevices()/refreshScreens().
}

void ControlPanel::syncPrefs() {
    prefs_.dbPath = dbEdit_->text().trimmed();
    prefs_.musicDir = dirEdit_->text().trimmed();
    prefs_.deviceIdx = deviceCombo_->currentIndex() > 0
                           ? deviceCombo_->currentData().toInt() : -1;
    prefs_.screenIdx = screenCombo_->currentIndex() > 0
                           ? screenCombo_->currentData().toInt() : -1;
    prefs_.match.noiseFloor = (float)noiseSpin_->value();
    prefs_.match.firstTrackAccept = (float)firstSpin_->value();
    prefs_.match.switchAccept = (float)switchSpin_->value();
    prefs_.match.confirmFrames = confirmSpin_->value();
    prefs_.save();
    if (controller_) controller_->setMatchParams(prefs_.match);
}

void ControlPanel::refreshDevices() {
    const int saved = prefs_.deviceIdx;
    deviceCombo_->blockSignals(true);
    deviceCombo_->clear();
    deviceCombo_->addItem(QStringLiteral("Default (system loopback)"), -1);
    int selectIdx = 0;
    int i = 0;
    for (const auto& d : WasapiCapture::listDevices()) {
        const QString name = QString::fromWCharArray(d.name.c_str());
        const QString tag = d.isLoopback ? QStringLiteral("[loopback] ")
                                         : QStringLiteral("[input] ");
        deviceCombo_->addItem(tag + name, d.index);
        if (d.index == saved) selectIdx = i;
        ++i;
    }
    deviceCombo_->setCurrentIndex(selectIdx);
    deviceCombo_->blockSignals(false);
}

void ControlPanel::refreshScreens() {
    const int saved = prefs_.screenIdx;
    screenCombo_->blockSignals(true);
    screenCombo_->clear();
    screenCombo_->addItem(QStringLiteral("Primary screen"), -1);
    int selectIdx = 0;
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QRect g = screens[i]->geometry();
        screenCombo_->addItem(QStringLiteral("[%1] %2  %3x%4")
                                  .arg(i)
                                  .arg(screens[i]->name())
                                  .arg(g.width()).arg(g.height()),
                              i);
        if (i == saved) selectIdx = i + 1;
    }
    screenCombo_->setCurrentIndex(selectIdx);
    screenCombo_->blockSignals(false);
}

void ControlPanel::refreshSongCount() {
    FpDb db;
    if (db.open(prefs_.dbPath.toStdString())) {
        songCountLabel_->setText(QString::number(db.listSongs().size()));
    } else {
        songCountLabel_->setText("—");
    }
}

void ControlPanel::browseDb() {
    const QString f = QFileDialog::getSaveFileName(
        this, t("dbPath"), dbEdit_->text(),
        QStringLiteral("SQLite DB (*.db)"));
    if (!f.isEmpty()) {
        dbEdit_->setText(QDir::toNativeSeparators(f));
        syncPrefs();
        refreshSongCount();
    }
}

void ControlPanel::browseMusicDir() {
    const QString d = QFileDialog::getExistingDirectory(
        this, t("musicDir"), dirEdit_->text());
    if (!d.isEmpty()) {
        dirEdit_->setText(QDir::toNativeSeparators(d));
        syncPrefs();
    }
}

void ControlPanel::startIndex() {
    if (indexing_.load()) return;
    const QString dir = dirEdit_->text().trimmed();
    const QString dbp = dbEdit_->text().trimmed();
    if (dbp.isEmpty()) { appendLog(t("needDb")); return; }
    if (dir.isEmpty()) { appendLog(t("needDir")); return; }
    syncPrefs();

    // Reap the previous worker before reassigning. A finished thread is
    // still joinable until joined, and move-assigning a joinable
    // std::thread calls std::terminate (crash on the second index run).
    if (indexThread_.joinable()) indexThread_.join();

    indexing_.store(true);
    indexBtn_->setEnabled(false);
    indexBtn_->setText(t("indexing"));
    indexBar_->setRange(0, 1);
    indexBar_->setValue(0);
    indexLabel_->setText(QString());

    // Cross-thread transport: emit queued signals at `this`.
    auto* self = this;
    indexThread_ = std::thread([self, dir, dbp] {
        // An uncaught exception in a worker thread = std::terminate.
        // Report failures back to the UI instead of killing the app.
        try {
            FpDb db;
            if (!db.open(dbp.toStdString())) {
                QMetaObject::invokeMethod(self, [self] {
                    self->appendLog(self->t("indexFailed"));
                    self->onIndexFinished(0, 0, 0);
                }, Qt::QueuedConnection);
                return;
            }
            Indexer indexer;
            IndexResult res = indexer.indexDirectory(dir.toStdString(), db, 0,
                [self](const IndexProgress& p) {
                    QMetaObject::invokeMethod(self, [self, p] {
                        self->onIndexProgress(p.done, p.total,
                                              QString::fromStdString(p.info));
                    }, Qt::QueuedConnection);
                });
            QMetaObject::invokeMethod(self, [self, res] {
                self->onIndexFinished(res.indexedOk, res.skipped, res.failed);
            }, Qt::QueuedConnection);
        } catch (const std::exception& e) {
            const QString msg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(self, [self, msg] {
                self->appendLog(self->t("indexFailed") + ": " + msg);
                self->onIndexFinished(0, 0, 0);
            }, Qt::QueuedConnection);
        } catch (...) {
            QMetaObject::invokeMethod(self, [self] {
                self->appendLog(self->t("indexFailed"));
                self->onIndexFinished(0, 0, 0);
            }, Qt::QueuedConnection);
        }
    });
}

void ControlPanel::onIndexProgress(int done, int total, const QString& info) {
    if (total > 0) {
        indexBar_->setRange(0, total);
        indexBar_->setValue(done);
    }
    indexLabel_->setText(QStringLiteral("%1 / %2  %3")
                             .arg(done).arg(total).arg(info));
}

void ControlPanel::onIndexFinished(int ok, int skipped, int failed) {
    indexing_.store(false);
    indexBtn_->setEnabled(true);
    indexBtn_->setText(t("index"));
    indexBar_->setRange(0, 100);
    indexBar_->setValue(100);
    indexLabel_->setText(QStringLiteral("%1: ok=%2 skipped=%3 failed=%4")
                             .arg(t("indexDone")).arg(ok).arg(skipped).arg(failed));
    refreshSongCount();
}

void ControlPanel::toggleViz() {
    if (controller_->isRunning()) {
        vizBtn_->setEnabled(false);
        controller_->stop();   // joins worker (GUI thread, ~30 ms worst case)
        return;
    }
    syncPrefs();
    const bool ok = controller_->start(
        prefs_.dbPath.toStdString(), prefs_.deviceIdx, prefs_.screenIdx);
    if (ok) {
        vizBtn_->setText(t("stopViz"));
        deviceCombo_->setEnabled(false);
        screenCombo_->setEnabled(false);
        dbEdit_->setEnabled(false);
        browseDbBtn_->setEnabled(false);
    }
}

void ControlPanel::onSessionStopped() {
    vizBtn_->setEnabled(true);
    vizBtn_->setText(t("startViz"));
    deviceCombo_->setEnabled(true);
    screenCombo_->setEnabled(true);
    dbEdit_->setEnabled(true);
    browseDbBtn_->setEnabled(true);
    levelBar_->setValue(0);
}

void ControlPanel::appendLog(const QString& line) {
    logView_->appendPlainText(line);
}

void ControlPanel::closeEvent(QCloseEvent* e) {
    if (indexing_.load()) appendLog(t("closing"));
    if (controller_) controller_->stop();
    if (indexThread_.joinable()) indexThread_.join();
    syncPrefs();
    levelTimer_->stop();
    e->accept();
}

} // namespace vj
