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
    void refreshScreens();
    void refreshSongCount();
    void browseDb();
    void browseMusicDir();
    void startIndex();
    void toggleViz();
    QString t(const char* key) const;

    std::unique_ptr<VizController> controller_;
    Prefs prefs_;

    // Audio / display
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* refreshDevBtn_ = nullptr;
    QComboBox* screenCombo_ = nullptr;

    // Library
    QLineEdit* dbEdit_ = nullptr;
    QPushButton* browseDbBtn_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QPushButton* browseDirBtn_ = nullptr;
    QPushButton* indexBtn_ = nullptr;
    QProgressBar* indexBar_ = nullptr;
    QLabel* indexLabel_ = nullptr;
    QLabel* songCountLabel_ = nullptr;

    // Thresholds
    QDoubleSpinBox* noiseSpin_ = nullptr;
    QDoubleSpinBox* firstSpin_ = nullptr;
    QDoubleSpinBox* switchSpin_ = nullptr;
    QSpinBox* confirmSpin_ = nullptr;

    // Run / meter / language / log
    QPushButton* vizBtn_ = nullptr;
    QProgressBar* levelBar_ = nullptr;
    QComboBox* langCombo_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* levelTimer_ = nullptr;

    std::thread indexThread_;
    std::atomic<bool> indexing_{false};

    QGroupBox* grpAudio_ = nullptr;
    QGroupBox* grpLib_ = nullptr;
    QGroupBox* grpThr_ = nullptr;
    QGroupBox* grpRun_ = nullptr;
};

} // namespace vj
