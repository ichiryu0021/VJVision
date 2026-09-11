// M4 control panel.
//
// One window, one shutdown path: closeEvent → hide → wait for any index
// thread → VizController::stop() (joins worker, tears down QML + IPC) →
// save prefs. All worker → UI updates cross threads through queued
// signals; the level meter polls VizController::peak() on a GUI timer.
#pragma once
#include "../viz/viz_controller.h"
#include "prefs.h"

#include <QWidget>
#include <QSlider>
#include <QLabel>
#include <atomic>
#include <memory>
#include <thread>

class QComboBox;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QTimer;
class QGroupBox;

namespace vj {

class ControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlPanel(QWidget* parent = nullptr);
    ~ControlPanel() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void appendLog(const QString& line);
    void onIndexProgress(int done, int total, const QString& info);
    void onIndexFinished(int ok, int skipped, int failed);
    void onSessionStopped();

private:
    void buildUi();
    void retranslate();
    void loadPrefsToUi();
    void syncPrefs();                 // UI → prefs_ + persist + controller
    void refreshDevices();
    void refreshSongCount();
    void refreshColorBtn();              // apply prefs_.bgColor to button
    void browseMusicDir();
    void startIndex();
    void cancelIndex();
    void forceReindex();    // v2.0.4: clear DB then startIndex()
    void toggleViz();
    QString t(const char* key) const;
    // Auto-discover VJVision.db inside dataDir
    QString resolveDbPath() const;

    std::unique_ptr<VizController> controller_;
    Prefs prefs_;

    // Audio hardware
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* refreshDevBtn_ = nullptr;
    QProgressBar* levelBar_ = nullptr;   // input level → moved here from Run

    // Database (data folder always <exe_dir>/data, music dir user-selectable)
    QLineEdit* dirEdit_ = nullptr;
    QPushButton* browseDirBtn_ = nullptr;
    QPushButton* indexBtn_ = nullptr;
    QPushButton* reindexBtn_ = nullptr;   // v2.0.4: force re-analyze
    QProgressBar* indexBar_ = nullptr;
    QLabel* indexLabel_ = nullptr;
    QLabel* songCountLabel_ = nullptr;

    // Visual / logo / background
    QLineEdit* standbyEdit_ = nullptr;
    QPushButton* browseStandbyBtn_ = nullptr;
    QPushButton* clearStandbyBtn_ = nullptr;
    QLineEdit* bgVideoEdit_ = nullptr;
    QPushButton* browseBgBtn_ = nullptr;
    QPushButton* clearBgBtn_ = nullptr;
    QComboBox* bgModeCombo_ = nullptr;
    QComboBox* fxTextureCombo_ = nullptr;   // v2.0.4: fx texture selector (pulse/ripple/particles)
    QComboBox* perfModeCombo_ = nullptr;     // v2.0.4: performance mode (auto/high/mid/low)
    QPushButton* bgColorBtn_ = nullptr;      // color picker for default-bg mode
    QLabel* bgRowLabel_ = nullptr;           // dynamic label: "bgColor" / "bgVideo"
    QSlider* overlaySlider_ = nullptr;       // replaces blur slider
    QLabel* overlayLabel_ = nullptr;          // shows "0%" ~ "100%"
    QComboBox* vizModeCombo_ = nullptr;      // 波形模式
    QSlider* logoStandbySlider_ = nullptr;  // standby logo size slider (×100)
    QLabel* logoStandbyLabel_ = nullptr;
    QSlider* logoPlayingSlider_ = nullptr;   // playing logo size slider (×100)
    QLabel* logoPlayingLabel_ = nullptr;

    // Thresholds
    QDoubleSpinBox* noiseSpin_ = nullptr;
    QDoubleSpinBox* firstSpin_ = nullptr;
    QDoubleSpinBox* switchSpin_ = nullptr;
    QSpinBox* confirmSpin_ = nullptr;

    // Run / language / log
    QPushButton* vizBtn_ = nullptr;
    QComboBox* langCombo_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* levelTimer_ = nullptr;

    std::thread indexThread_;
    std::atomic<bool> indexing_{false};
    std::atomic<bool> cancelFlag_{false};

    QGroupBox* grpAudio_ = nullptr;
    QGroupBox* grpLib_ = nullptr;
    QGroupBox* grpVisual_ = nullptr;
    QGroupBox* grpThr_ = nullptr;
    QGroupBox* grpRun_ = nullptr;
};

} // namespace vj
