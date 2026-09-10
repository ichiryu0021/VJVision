// Owns one running visualizer session: the Qt Quick fullscreen window
// (QtVizSink), the IPC pipe outlet, and the capture/spectrum/recognition
// worker thread. The control panel (M4) starts/stops this; the CLI
// `viz` command uses the same class.
//
// Threading: start()/stop() run on the GUI thread. The worker emits
// logMessage() (auto-connection → queued onto the GUI thread) and pushes
// viz events through MulticastSink. Recognition thresholds can be
// changed live via setMatchParams() while a session is running.
#pragma once
#include "viz_events.h"
#include "../engine/match_engine.h"

#include <QObject>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vj {

class QtVizSink;
class IpcVizSink;

class VizController : public QObject {
    Q_OBJECT
public:
    explicit VizController(QObject* parent = nullptr);
    ~VizController() override;

    // Creates the QML fullscreen window on screenIndex (-1 = primary)
    // and starts capture/recognition. Must be called on the GUI thread.
    // Returns false (and logs the reason) if the DB is missing/empty,
    // the QML window fails to load, or a session is already running.
    bool start(const std::string& dbPath, int deviceIndex, int screenIndex);

    // Signals the worker to stop and joins it, tears down the QML window
    // and IPC. Idempotent; safe to call from closeEvent.
    void stop();

    bool isRunning() const { return running_.load(); }

    // Latest capture peak 0..1 (written on the GUI thread by the sink's
    // queued slot, so reading it from GUI timers is safe).
    float peak() const;

    // Live recognition thresholds.
    void setMatchParams(const MatchParams& p);
    MatchParams matchParams() const;

signals:
    // Emitted from the worker thread; arrives queued on the GUI thread.
    void logMessage(QString line);
    void sessionStarted();
    void sessionStopped();

private:
    void workerFunc(std::string dbPath, int deviceIndex);

    std::atomic<bool> running_{false};
    std::thread worker_;

    std::shared_ptr<QtVizSink> qtSink_;
    std::shared_ptr<IpcVizSink> ipcSink_;
    std::shared_ptr<MulticastSink> multicast_;

    mutable std::mutex paramsMtx_;
    MatchParams params_;
};

} // namespace vj
