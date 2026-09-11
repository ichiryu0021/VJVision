#include "control_panel.h"

#include "../audio/wasapi_capture.h"
#include "../engine/indexer.h"
#include "../fp/fp_db.h"

#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace vj {

namespace {
const char* tr2(const QString& lang, const char* key) {
    static const struct { const char* k; const char* zh; const char* en; } rows[] = {
        {"appTitle",       "VJVision 控制面板",            "VJVision Control Panel"},
        {"grpAudio",       "音频硬件",                     "Audio Hardware"},
        {"device",         "音频设备",                     "Audio device"},
        {"refresh",        "刷新",                         "Refresh"},
        {"level",          "输入电平",                     "Input level"},
        {"grpLib",         "数据库",                       "Database"},
        {"dataDir",        "数据文件夹",                   "Data folder"},
        {"musicDir",       "音乐目录",                     "Music directory"},
        {"browse",         "浏览…",                        "Browse…"},
        {"index",          "开始分析",                     "Start analysis"},
        {"indexing",       "分析中…",                      "Analyzing…"},
        {"cancelIndex",    "取消分析",                     "Cancel analysis"},
        {"indexCancelled", "分析已取消",                   "Analysis cancelled"},
        {"forceReindex",   "强制重新分析",                 "Force re-analyze"},
        {"reindexConfirm", "将清除所有已分析数据并重新开始。确定继续？",
                           "This will clear ALL analyzed data and restart. Continue?"},
        {"dbCleared",      "数据库已清空，开始重新分析…",  "Database cleared, re-analyzing…"},
        {"indexRun",       "分析音乐",                     "Analyze music"},
        {"indexStatus",    "分析状态",                     "Analysis status"},
        {"songCount",      "已分析歌曲",                   "Analyzed songs"},
        {"grpVisual",      "视觉效果",                     "Visual effects"},
        {"standby",        "待机 Logo",                    "Standby logo"},
        {"bgVideo",        "背景媒体",                     "Background media"},
        {"clear",          "清除",                         "Clear"},
        {"bgColor",        "底色",                         "Bg color"},
        {"bgOverlay",      "黑色遮罩",                     "Dark overlay"},
        {"bgMode",         "背景来源",                     "Bg source"},
        {"bgDefault",      "默认（内置）",                  "Default (built-in)"},
        {"bgCustom",       "自定义",                       "Custom"},
        {"bgFx",           "节奏纹理",                     "Rhythm texture"},
        {"fxTexture",      "纹理",                         "Texture"},
        {"fxPulse",        "律动波场",                     "Pulse"},
        {"fxRipple",       "同心涟漪",                     "Ripple"},
        {"fxParticles",    "粒子迸发",                     "Particles"},
        {"perfMode",       "性能模式",                     "Performance"},
        {"perfAuto",       "自动",                         "Auto"},
        {"perfHigh",       "高",                           "High"},
        {"perfMid",        "中",                           "Mid"},
        {"perfLow",        "低",                           "Low"},
        {"vizMode",        "波形模式",                     "Waveform mode"},
        {"vizMirrored",    "镜像柱状",                     "Mirrored bars"},
        {"vizCentered",    "居中柱状",                     "Centered bars"},
        {"vizRadial",      "放射状",                       "Radial"},
        {"vizWaterfall",   "瀑布图",                       "Waterfall"},
        {"logoStandby",    "待机 Logo 大小",               "Standby logo size"},
        {"logoPlaying",    "播放 Logo 大小",               "Playing logo size"},
        {"resetSize",      "默认",                         "Default"},
        {"grpThr",         "识别阈值（即时生效）",         "Recognition thresholds (live)"},
        {"noise",          "噪声下限",                     "Noise floor"},
        {"first",          "首曲确认",                     "First-track accept"},
        {"switch",         "切歌确认",                     "Switch accept"},
        {"confirm",        "确认次数",                     "Confirm frames"},
        {"resetDefault",   "恢复默认",                     "Reset defaults"},
        {"grpRun",         "运行",                         "Run"},
        {"startViz",       "启动可视化",                   "Start visualizer"},
        {"stopViz",        "停止可视化",                   "Stop visualizer"},
        {"resetViz",       "重置并启动",                   "Reset & start"},
        {"language",       "语言",                         "Language"},
        {"log",            "日志",                         "Log"},
        {"needDir",        "请先选择音乐目录",             "Pick a music directory first"},
        {"indexDone",      "分析完成",                     "Analysis complete"},
        {"indexFailed",    "分析失败：无法打开数据库",     "Analysis failed: cannot open DB"},
        {"closing",        "正在关闭，等待分析线程结束…",  "Closing, waiting for analyzer…"},
        {"noDb",           "数据文件夹中无 VJVision.db — 运行可视化不会有曲目匹配",
                           "No VJVision.db in data folder — viz runs without track matching"},
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
    if (prefs_.dataDir.isEmpty())
        prefs_.dataDir = Prefs::defaultDataDir();

    buildUi();
    loadPrefsToUi();
    retranslate();
    refreshDevices();
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

    appendLog(QStringLiteral("VJVision control panel ready. Data folder: %1").arg(prefs_.dataDir));
}

ControlPanel::~ControlPanel() = default;

QString ControlPanel::t(const char* key) const {
    return QString::fromUtf8(tr2(prefs_.language, key));
}

QString ControlPanel::resolveDbPath() const {
    if (prefs_.dataDir.isEmpty()) return QString();
    QDir dir(prefs_.dataDir);
    QString db = dir.filePath(QStringLiteral("VJVision.db"));
    if (QFileInfo::exists(db)) return db;
    return QString();
}

void ControlPanel::buildUi() {
    setMinimumWidth(760);
    auto* root = new QVBoxLayout(this);

    auto lbl = [](const char* key) {
        auto* l = new QLabel;
        l->setObjectName(QString::fromUtf8(key));
        return l;
    };

    // --- Audio hardware -------------------------------------------------
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
    levelBar_ = new QProgressBar;
    levelBar_->setRange(0, 100);
    levelBar_->setValue(0);
    levelBar_->setFormat(QStringLiteral("PEAK %p%"));
    audioForm->addRow(lbl("level"), levelBar_);
    root->addWidget(grpAudio_);

    // --- Database -------------------------------------------------------
    grpLib_ = new QGroupBox;
    auto* libForm = new QFormLayout(grpLib_);
    auto* dirRow = new QHBoxLayout();
    dirEdit_ = new QLineEdit;
    browseDirBtn_ = new QPushButton;
    connect(browseDirBtn_, &QPushButton::clicked, this, &ControlPanel::browseMusicDir);
    dirRow->addWidget(dirEdit_, 1);
    dirRow->addWidget(browseDirBtn_);
    libForm->addRow(lbl("musicDir"), dirRow);
    auto* idxRow = new QHBoxLayout();
    indexBtn_ = new QPushButton;
    connect(indexBtn_, &QPushButton::clicked, this, [this] {
        if (indexing_.load()) cancelIndex();
        else startIndex();
    });
    reindexBtn_ = new QPushButton;
    connect(reindexBtn_, &QPushButton::clicked, this, &ControlPanel::forceReindex);
    indexBar_ = new QProgressBar;
    indexBar_->setRange(0, 100);
    indexBar_->setValue(0);
    idxRow->addWidget(indexBtn_);
    idxRow->addWidget(reindexBtn_);
    idxRow->addWidget(indexBar_, 1);
    libForm->addRow(lbl("indexRun"), idxRow);
    indexLabel_ = new QLabel;
    indexLabel_->setObjectName("indexStatusValue");
    songCountLabel_ = new QLabel(QStringLiteral("—"));
    libForm->addRow(lbl("indexStatus"), indexLabel_);
    libForm->addRow(lbl("songCount"), songCountLabel_);
    root->addWidget(grpLib_);

    // --- Visual effects -------------------------------------------------
    grpVisual_ = new QGroupBox;
    auto* visForm = new QFormLayout(grpVisual_);
    auto* standbyRow = new QHBoxLayout();
    standbyEdit_ = new QLineEdit;
    browseStandbyBtn_ = new QPushButton;
    clearStandbyBtn_ = new QPushButton;
    connect(browseStandbyBtn_, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, tr2(prefs_.language, "standby"),
            standbyEdit_->text(), QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
        if (!f.isEmpty()) {
            standbyEdit_->setText(QDir::toNativeSeparators(f));
            syncPrefs();
        }
    });
    connect(clearStandbyBtn_, &QPushButton::clicked, this, [this] {
        standbyEdit_->clear();
        syncPrefs();
    });
    standbyRow->addWidget(standbyEdit_, 1);
    standbyRow->addWidget(browseStandbyBtn_);
    standbyRow->addWidget(clearStandbyBtn_);
    visForm->addRow(lbl("standby"), standbyRow);

    // --- Logo size sliders (right after standby row) ---
    {
        auto* row1 = new QHBoxLayout;
        logoStandbySlider_ = new QSlider(Qt::Horizontal);
        logoStandbySlider_->setRange(30, 150);          // 0.30× – 1.50×, stored ×100
        // setValue AFTER setRange, but blockSignals to avoid any spurious valueChanged
        logoStandbySlider_->blockSignals(true);
        logoStandbySlider_->setValue(100);               // 100 → 1.00× default
        logoStandbySlider_->blockSignals(false);
        logoStandbyLabel_ = new QLabel("1.00×");
        logoStandbyLabel_->setFixedWidth(45);
        auto* resetStandbyBtn = new QPushButton(t("resetSize"));
        resetStandbyBtn->setFixedWidth(50);
        row1->addWidget(logoStandbySlider_, 1);
        row1->addWidget(logoStandbyLabel_);
        row1->addWidget(resetStandbyBtn);
        visForm->addRow(lbl("logoStandby"), row1);

        auto* row2 = new QHBoxLayout;
        logoPlayingSlider_ = new QSlider(Qt::Horizontal);
        logoPlayingSlider_->setRange(5, 80);             // 0.05× – 0.80×, stored ×100
        logoPlayingSlider_->blockSignals(true);
        logoPlayingSlider_->setValue(30);                // 30 → 0.30× default
        logoPlayingSlider_->blockSignals(false);
        logoPlayingLabel_ = new QLabel("0.30×");
        logoPlayingLabel_->setFixedWidth(45);
        auto* resetPlayingBtn = new QPushButton(t("resetSize"));
        resetPlayingBtn->setFixedWidth(50);
        row2->addWidget(logoPlayingSlider_, 1);
        row2->addWidget(logoPlayingLabel_);
        row2->addWidget(resetPlayingBtn);
        visForm->addRow(lbl("logoPlaying"), row2);

        auto updateStandbyLbl = [this](int v) {
            logoStandbyLabel_->setText(QStringLiteral("%1×").arg(v / 100.0, 0, 'f', 2));
        };
        auto updatePlayingLbl = [this](int v) {
            logoPlayingLabel_->setText(QStringLiteral("%1×").arg(v / 100.0, 0, 'f', 2));
        };
        connect(logoStandbySlider_, &QSlider::valueChanged, this, [this, updateStandbyLbl](int v) {
            updateStandbyLbl(v); syncPrefs();
        });
        connect(logoPlayingSlider_, &QSlider::valueChanged, this, [this, updatePlayingLbl](int v) {
            updatePlayingLbl(v); syncPrefs();
        });
        connect(resetStandbyBtn, &QPushButton::clicked, this, [this] {
            logoStandbySlider_->setValue(100);
        });
        connect(resetPlayingBtn, &QPushButton::clicked, this, [this] {
            logoPlayingSlider_->setValue(30);
        });
    }
    // Background source mode: Default (built-in) / Custom / Rhythm texture (fx)
    auto* bgModeCombo = new QComboBox;
    bgModeCombo->addItem(t("bgDefault"), 0);
    bgModeCombo->addItem(t("bgCustom"), 1);
    bgModeCombo->addItem(t("bgFx"), 2);
    bgModeCombo_ = bgModeCombo;
    visForm->addRow(lbl("bgMode"), bgModeCombo);

    // v2.0.4: fx texture + performance mode selectors (shown when bgMode==2)
    fxTextureCombo_ = new QComboBox;
    fxTextureCombo_->addItem(t("fxPulse"), 0);
    fxTextureCombo_->addItem(t("fxRipple"), 1);
    fxTextureCombo_->addItem(t("fxParticles"), 2);
    connect(fxTextureCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        prefs_.fxTexture = idx; syncPrefs();
        if (controller_ && controller_->isRunning())
            controller_->setFxTexture(prefs_.fxTexture);
    });
    perfModeCombo_ = new QComboBox;
    perfModeCombo_->addItem(t("perfAuto"), 0);
    perfModeCombo_->addItem(t("perfHigh"), 1);
    perfModeCombo_->addItem(t("perfMid"), 2);
    perfModeCombo_->addItem(t("perfLow"), 3);
    connect(perfModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        prefs_.performanceMode = idx; syncPrefs();
        if (controller_ && controller_->isRunning())
            controller_->setPerformanceMode(prefs_.performanceMode);
    });

    // Row label — changes: "bgColor" (Default) or "bgVideo" (Custom)
    bgRowLabel_ = new QLabel;

    // Row that morphs between: Default → color picker button, Custom → file browse
    auto* bgRow = new QHBoxLayout();
    bgColorBtn_ = new QPushButton;
    bgColorBtn_->setFixedWidth(80);
    bgVideoEdit_ = new QLineEdit;
    browseBgBtn_ = new QPushButton;
    clearBgBtn_ = new QPushButton;
    refreshColorBtn();
    connect(bgColorBtn_, &QPushButton::clicked, this, [this] {
        QColor current(prefs_.bgColor);
        QColor c = QColorDialog::getColor(current, this, tr2(prefs_.language, "bgColor"));
        if (c.isValid()) {
            prefs_.bgColor = c.name();
            refreshColorBtn();
            syncPrefs();
        }
    });
    connect(browseBgBtn_, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, tr2(prefs_.language, "bgVideo"),
            bgVideoEdit_->text(),
            QStringLiteral("Background (*.gif *.webp *.png *.jpg *.jpeg *.mp4 *.mov *.mkv *.avi)"));
        if (!f.isEmpty()) {
            bgVideoEdit_->setText(QDir::toNativeSeparators(f));
            syncPrefs();
        }
    });
    connect(clearBgBtn_, &QPushButton::clicked, this, [this] {
        bgVideoEdit_->clear();
        syncPrefs();
    });
    // Dynamic row (label + contents)
    auto* bgRowWidget = new QWidget;
    bgRowWidget->setLayout(bgRow);
    const QString labelBgVideo = lbl("bgVideo")->text();
    const QString labelBgColor = lbl("bgColor")->text();
    const QString labelFxTexture = lbl("fxTexture")->text();
    const QString labelPerfMode = lbl("perfMode")->text();
    auto updateBgRowForMode = [bgRow, this, labelBgVideo, labelBgColor,
                               labelFxTexture, labelPerfMode](int mode) {
        // Clear layout
        QLayoutItem* item;
        while ((item = bgRow->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->setParent(nullptr);
            delete item;
        }
        if (mode == 1) {
            // Custom background media
            bgRowLabel_->setText(labelBgVideo);
            bgRow->addWidget(bgVideoEdit_, 1);
            bgRow->addWidget(browseBgBtn_);
            bgRow->addWidget(clearBgBtn_);
        } else if (mode == 2) {
            // Rhythm texture (fx) — show texture + perf selectors side by side
            bgRowLabel_->setText(labelFxTexture);
            bgRow->addWidget(fxTextureCombo_, 1);
            auto* perfLabel = new QLabel(labelPerfMode);
            bgRow->addWidget(perfLabel);
            bgRow->addWidget(perfModeCombo_, 1);
        } else {
            // Default (built-in) — color picker
            bgRowLabel_->setText(labelBgColor);
            bgRow->addWidget(bgColorBtn_);
            bgRow->addStretch(1);
        }
    };
    visForm->addRow(bgRowLabel_, bgRowWidget);

    // Overlay depth slider (0 = no dim, 1 = fully black) — ALWAYS enabled
    auto* overlayRow = new QHBoxLayout();
    overlaySlider_ = new QSlider(Qt::Horizontal);
    overlaySlider_->setRange(0, 100);
    overlaySlider_->setSingleStep(1);
    overlayLabel_ = new QLabel;
    overlayLabel_->setFixedWidth(40);
    overlayLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(overlaySlider_, &QSlider::valueChanged, this, [this](int v) {
        float f = v / 100.0f;
        overlayLabel_->setText(QStringLiteral("%1%").arg(v));
        prefs_.bgOverlayDepth = f;
        syncPrefs();
    });
    overlayRow->addWidget(overlaySlider_, 1);
    overlayRow->addWidget(overlayLabel_);
    visForm->addRow(lbl("bgOverlay"), overlayRow);

    // Waveform / spectrum visualization mode
    vizModeCombo_ = new QComboBox;
    vizModeCombo_->addItem(t("vizMirrored"), 0);
    vizModeCombo_->addItem(t("vizRadial"), 1);
    vizModeCombo_->addItem(t("vizWaterfall"), 2);
    connect(vizModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
        prefs_.vizMode = idx;
        syncPrefs();
    });
    visForm->addRow(lbl("vizMode"), vizModeCombo_);

    connect(bgModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, updateBgRowForMode](int m) {
                updateBgRowForMode(m);
                bool custom = (m == 1);
                overlaySlider_->setEnabled(custom);
                overlayLabel_->setEnabled(custom);
            });
    // Initial state — Default mode disables overlay slider
    overlaySlider_->setEnabled(false);
    overlayLabel_->setEnabled(false);
    updateBgRowForMode(bgModeCombo->currentIndex());
    root->addWidget(grpVisual_);

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
    confirmSpin_->setValue(1);
    thrForm->addRow(lbl("noise"), noiseSpin_);
    thrForm->addRow(lbl("first"), firstSpin_);
    thrForm->addRow(lbl("switch"), switchSpin_);
    thrForm->addRow(lbl("confirm"), confirmSpin_);
    // Reset-to-default button
    auto* resetRow = new QHBoxLayout;
    resetRow->addStretch();
    auto* resetBtn = new QPushButton;
    resetBtn->setText(t("resetDefault"));
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        noiseSpin_->setValue(0.13);
        firstSpin_->setValue(0.25);
        switchSpin_->setValue(0.30);
        confirmSpin_->setValue(1);
        syncPrefs();
    });
    resetRow->addWidget(resetBtn);
    thrForm->addRow(resetRow);
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
    langCombo_->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    langCombo_->addItem(QStringLiteral("English"), QStringLiteral("en"));
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
    grpVisual_->setTitle(t("grpVisual"));
    grpThr_->setTitle(t("grpThr"));
    grpRun_->setTitle(t("grpRun"));
    refreshDevBtn_->setText(t("refresh"));
    browseDirBtn_->setText(t("browse"));
    browseStandbyBtn_->setText(t("browse"));
    browseBgBtn_->setText(t("browse"));
    clearStandbyBtn_->setText(t("clear"));
    clearBgBtn_->setText(t("clear"));
    indexBtn_->setText(indexing_.load() ? t("cancelIndex") : t("index"));
    reindexBtn_->setText(t("forceReindex"));
    reindexBtn_->setEnabled(!indexing_.load());
    vizModeCombo_->setItemText(0, t("vizMirrored"));
    vizModeCombo_->setItemText(1, t("vizRadial"));
    vizModeCombo_->setItemText(2, t("vizWaterfall"));
    bgModeCombo_->setItemText(0, t("bgDefault"));
    bgModeCombo_->setItemText(1, t("bgCustom"));
    bgModeCombo_->setItemText(2, t("bgFx"));
    fxTextureCombo_->setItemText(0, t("fxPulse"));
    fxTextureCombo_->setItemText(1, t("fxRipple"));
    fxTextureCombo_->setItemText(2, t("fxParticles"));
    perfModeCombo_->setItemText(0, t("perfAuto"));
    perfModeCombo_->setItemText(1, t("perfHigh"));
    perfModeCombo_->setItemText(2, t("perfMid"));
    perfModeCombo_->setItemText(3, t("perfLow"));
    if (controller_ && controller_->isRunning()) {
        vizBtn_->setText(t("stopViz"));
    } else {
        vizBtn_->setText(t("startViz"));
    }

    for (QLabel* l : findChildren<QLabel*>()) {
        const QByteArray key = l->objectName().toUtf8();
        if (key.isEmpty() || key == "indexStatusValue") continue;
        const char* txt = tr2(prefs_.language, key.constData());
        if (qstrcmp(txt, key.constData()) != 0)
            l->setText(QString::fromUtf8(txt));
    }
}

void ControlPanel::loadPrefsToUi() {
    dirEdit_->setText(prefs_.musicDir);
    standbyEdit_->setText(prefs_.standbyPath);
    bgVideoEdit_->setText(prefs_.bgVideoPath);
    bgModeCombo_->setCurrentIndex(prefs_.bgMode);
    fxTextureCombo_->setCurrentIndex(prefs_.fxTexture);
    perfModeCombo_->setCurrentIndex(prefs_.performanceMode);
    overlaySlider_->setValue(int(qBound(0.f, prefs_.bgOverlayDepth, 1.f) * 100.f));
    overlayLabel_->setText(QStringLiteral("%1%").arg(overlaySlider_->value()));
    noiseSpin_->setValue(prefs_.match.noiseFloor);
    firstSpin_->setValue(prefs_.match.firstTrackAccept);
    switchSpin_->setValue(prefs_.match.switchAccept);
    confirmSpin_->setValue(prefs_.match.confirmFrames);
    vizModeCombo_->setCurrentIndex(prefs_.vizMode);
    logoStandbySlider_->blockSignals(true);
    logoStandbySlider_->setValue(int(prefs_.logoSizeStandby * 100));
    logoStandbySlider_->blockSignals(false);
    logoStandbyLabel_->setText(QStringLiteral("%1×").arg(logoStandbySlider_->value() / 100.0, 0, 'f', 2));
    logoPlayingSlider_->blockSignals(true);
    logoPlayingSlider_->setValue(int(prefs_.logoSizePlaying * 100));
    logoPlayingSlider_->blockSignals(false);
    logoPlayingLabel_->setText(QStringLiteral("%1×").arg(logoPlayingSlider_->value() / 100.0, 0, 'f', 2));
    refreshColorBtn();
    const int li = langCombo_->findData(prefs_.language);
    if (li >= 0) langCombo_->setCurrentIndex(li);
}

void ControlPanel::syncPrefs() {
    prefs_.musicDir = dirEdit_->text().trimmed();
    prefs_.standbyPath = standbyEdit_->text().trimmed();
    prefs_.bgMode = bgModeCombo_->currentIndex();   // 0=default, 1=custom, 2=fx
    prefs_.fxTexture = fxTextureCombo_->currentIndex();
    prefs_.performanceMode = perfModeCombo_->currentIndex();
    prefs_.bgVideoPath = bgVideoEdit_->text().trimmed();
    prefs_.deviceIdx = deviceCombo_->currentIndex() > 0
                           ? deviceCombo_->currentData().toInt() : -1;
    prefs_.match.noiseFloor = (float)noiseSpin_->value();
    prefs_.match.firstTrackAccept = (float)firstSpin_->value();
    prefs_.match.switchAccept = (float)switchSpin_->value();
    prefs_.match.confirmFrames = confirmSpin_->value();
    prefs_.bgColor = bgColorBtn_->property("color").toString();
    prefs_.vizMode = vizModeCombo_->currentIndex();
    prefs_.logoSizeStandby = logoStandbySlider_->value() / 100.0f;
    prefs_.logoSizePlaying = logoPlayingSlider_->value() / 100.0f;
    prefs_.save();
    if (controller_) {
        controller_->setMatchParams(prefs_.match);
        // Standby logo: user path takes priority over auto-discovered
        if (!prefs_.standbyPath.isEmpty()) {
            QString url = QStringLiteral("file:///") + QDir::toNativeSeparators(prefs_.standbyPath).replace('\\', '/');
            controller_->setStandbyPath(url);
        }
        // Bg video: only push custom path when user explicitly chose Custom mode
        controller_->setBgVideoPath((prefs_.bgMode == 1)
            ? QStringLiteral("file:///") + QDir::toNativeSeparators(prefs_.bgVideoPath).replace('\\', '/')
            : QString());
        controller_->setBgOverlayDepth(prefs_.bgOverlayDepth);
        controller_->setBgColor(prefs_.bgColor);
        controller_->setVizMode(prefs_.vizMode);
        controller_->setBgMode(prefs_.bgMode);
        controller_->setFxTexture(prefs_.fxTexture);
        controller_->setPerformanceMode(prefs_.performanceMode);
        controller_->setLogoSizeStandby(prefs_.logoSizeStandby);
        controller_->setLogoSizePlaying(prefs_.logoSizePlaying);
    }
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

void ControlPanel::refreshSongCount() {
    QString db = resolveDbPath();
    if (db.isEmpty()) {
        songCountLabel_->setText(QStringLiteral("—"));
        return;
    }
    FpDb fpdb;
    if (fpdb.open(db.toStdString())) {
        songCountLabel_->setText(QString::number(fpdb.listSongs().size()));
    } else {
        songCountLabel_->setText(QStringLiteral("—"));
    }
}

void ControlPanel::refreshColorBtn() {
    QColor c(prefs_.bgColor);
    if (prefs_.bgColor.isEmpty()) { c = "#000000"; prefs_.bgColor = "#000000"; }
    QColor hover = c.lighter(115);
    QString tc = (c.lightness() > 128) ? "#000" : "#fff";
    bgColorBtn_->setText(prefs_.bgColor);
    bgColorBtn_->setProperty("color", prefs_.bgColor);
    bgColorBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid rgba(128,128,128,80); border-radius: 3px; padding: 2px 6px; }"
        "QPushButton:hover { background-color: %3; border: 1px solid rgba(180,180,180,140); }"
        "QPushButton:pressed { background-color: %1; }")
        .arg(prefs_.bgColor, tc, hover.name()));
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
    syncPrefs();
    const QString dbp = resolveDbPath();
    const QString dir = dirEdit_->text().trimmed();
    if (dir.isEmpty()) { appendLog(t("needDir")); return; }

    if (indexThread_.joinable()) indexThread_.join();

    cancelFlag_.store(false);
    indexing_.store(true);
    indexBtn_->setText(t("cancelIndex"));
    reindexBtn_->setEnabled(false);
    indexBar_->setRange(0, 1);
    indexBar_->setValue(0);
    indexLabel_->setText(QString());

    auto* self = this;
    // If no pre-existing DB, use <dataDir>/VJVision.db — Indexer will create it.
    QString finalDb = dbp;
    if (finalDb.isEmpty() && !prefs_.dataDir.isEmpty()) {
        QDir d(prefs_.dataDir);
        finalDb = d.filePath(QStringLiteral("VJVision.db"));
    }

    indexThread_ = std::thread([self, dir, finalDb] {
        try {
            FpDb db;
            if (!db.open(finalDb.toStdString())) {
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
                }, &self->cancelFlag_);
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

void ControlPanel::cancelIndex() {
    if (!indexing_.load()) return;
    cancelFlag_.store(true);
    indexBtn_->setEnabled(false);
    reindexBtn_->setEnabled(false);
    appendLog(t("indexCancelled"));
}

void ControlPanel::forceReindex() {
    // v2.0.4: clear ALL fingerprint data then re-analyze the current music
    // directory from scratch. Uses the current dirEdit_ value (no folder
    // picker) — only a confirmation dialog, per legacy V1 constraint.
    if (indexing_.load()) return;   // sanity: button is disabled during indexing
    syncPrefs();
    const QString dir = dirEdit_->text().trimmed();
    if (dir.isEmpty()) { appendLog(t("needDir")); return; }

    // Confirmation dialog — destructive operation.
    auto reply = QMessageBox::question(
        this, t("forceReindex"), t("reindexConfirm"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    // Resolve DB path (same logic as startIndex).
    QString finalDb = resolveDbPath();
    if (finalDb.isEmpty() && !prefs_.dataDir.isEmpty()) {
        QDir d(prefs_.dataDir);
        finalDb = d.filePath(QStringLiteral("VJVision.db"));
    }
    if (!finalDb.isEmpty()) {
        FpDb db;
        if (db.open(finalDb.toStdString())) {
            db.clear();      // DROP + recreate songs/fingerprints tables
            db.vacuum();     // reclaim space
            db.close();
        }
    }
    appendLog(t("dbCleared"));
    refreshSongCount();   // show "0" before re-analysis starts
    startIndex();         // DB is now empty → every file gets re-indexed
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
    bool wasCancelled = cancelFlag_.load();
    indexing_.store(false);
    indexBtn_->setEnabled(true);
    indexBtn_->setText(t("index"));
    reindexBtn_->setEnabled(true);
    indexBar_->setRange(0, 100);
    indexBar_->setValue(100);
    if (wasCancelled) {
        indexLabel_->setText(QStringLiteral("%1: ok=%2 skipped=%3 failed=%4")
                                 .arg(t("indexCancelled")).arg(ok).arg(skipped).arg(failed));
    } else {
        indexLabel_->setText(QStringLiteral("%1: ok=%2 skipped=%3 failed=%4")
                                 .arg(t("indexDone")).arg(ok).arg(skipped).arg(failed));
    }
    refreshSongCount();
}

void ControlPanel::toggleViz() {
    if (controller_->isRunning()) {
        vizBtn_->setEnabled(false);
        controller_->stop();
        return;
    }
    // Clicking after stop() now starts fresh — controller_ is fully reset.
    syncPrefs();   // saves prefs to disk (qtSink_ doesn't exist yet → push is skipped)
    QString db = resolveDbPath();
    if (db.isEmpty()) {
        appendLog(t("noDb"));
    }
    const std::string dbStr = db.toStdString();
    const bool ok = controller_->start(dbStr, prefs_.deviceIdx);
    if (ok) {
        vizBtn_->setText(t("stopViz"));
        deviceCombo_->setEnabled(false);
        // NOW qtSink_ exists — push all custom paths + effects to override auto-discovered defaults
        if (!prefs_.standbyPath.isEmpty()) {
            QString url = QStringLiteral("file:///") + QDir::toNativeSeparators(prefs_.standbyPath).replace('\\', '/');
            appendLog(QStringLiteral("[push] standby = %1").arg(url));
            controller_->setStandbyPath(url);
        } else {
            appendLog(QStringLiteral("[push] standby = (none)"));
        }
        QString bgUrl;
        if (prefs_.bgMode == 1 && !prefs_.bgVideoPath.isEmpty()) {
            bgUrl = QStringLiteral("file:///") + QDir::toNativeSeparators(prefs_.bgVideoPath).replace('\\', '/');
            appendLog(QStringLiteral("[push] bgVideo(custom) = %1").arg(bgUrl));
        } else {
            appendLog(QStringLiteral("[push] bgVideo = (default/off)  bgMode=%1  path=%2")
                .arg(prefs_.bgMode).arg(prefs_.bgVideoPath));
        }
        controller_->setBgVideoPath(bgUrl);
        controller_->setBgOverlayDepth(prefs_.bgOverlayDepth);
        controller_->setBgColor(prefs_.bgColor);
        controller_->setVizMode(prefs_.vizMode);
        controller_->setBgMode(prefs_.bgMode);
        controller_->setFxTexture(prefs_.fxTexture);
        controller_->setPerformanceMode(prefs_.performanceMode);
        appendLog(QStringLiteral("[push] bgOverlayDepth = %1  bgColor = %2  vizMode = %3  bgMode = %4")
            .arg(prefs_.bgOverlayDepth, 0, 'f', 2).arg(prefs_.bgColor)
            .arg(prefs_.vizMode).arg(prefs_.bgMode));
    }
}

void ControlPanel::onSessionStopped() {
    vizBtn_->setEnabled(true);
    retranslate();   // picks correct startViz text
    deviceCombo_->setEnabled(true);
    levelBar_->setValue(0);
}

void ControlPanel::appendLog(const QString& line) {
    logView_->appendPlainText(line);
}

void ControlPanel::closeEvent(QCloseEvent* e) {
    if (indexing_.load()) {
        appendLog(t("closing"));
        cancelFlag_.store(true);
    }
    if (controller_) controller_->stop();
    if (indexThread_.joinable()) indexThread_.join();
    syncPrefs();
    levelTimer_->stop();
    e->accept();
}

} // namespace vj
