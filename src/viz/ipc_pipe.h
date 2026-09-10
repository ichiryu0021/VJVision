// IPC outlet for an external renderer (e.g. a future Unity player).
//
// Runs a Windows named-pipe server (\\.\pipe\vjvcplus_viz). Any number of
// clients may connect; each receives every event as one newline-terminated
// JSON object:
//   {"type":"track","valid":true,"title":"...","artist":"...",
//    "album":"...","confidence":0.42,"tentative":false}
//   {"type":"spectrum","bins":[0.12,...64 values...],"peak":0.83}
//   {"type":"status","state":"standby|listening|matching|mixing"}
// Clients are write-only from our side; slow/disconnected clients are
// dropped without affecting the in-process UI.
#pragma once
#include "viz_events.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace vj {

class IpcVizSink : public VizSink {
public:
    IpcVizSink();
    ~IpcVizSink() override;

    bool start();   // begins the acceptor thread
    void stop();

    void onTrack(const TrackEvent& t) override;
    void onSpectrum(const float* bins, int count, float peak) override;
    void onStatus(VizStatus status) override;

    int clientCount();

private:
    struct Client {
        void* handle = nullptr;      // HANDLE (void* to avoid windows.h in header)
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::thread writer;
        std::atomic<bool> alive{true};
    };

    void acceptorLoop();
    void writerLoop(Client* c);
    void broadcast(std::string msg);
    static std::string jsonEscape(const std::string& s);

    std::atomic<bool> running_{false};
    std::thread acceptor_;
    std::mutex clientsMtx_;
    std::vector<Client*> clients_;
};

} // namespace vj
