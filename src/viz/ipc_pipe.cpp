#include "ipc_pipe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <sstream>

namespace vj {

static const char* kPipeName = "\\\\.\\pipe\\VJVision_viz";

IpcVizSink::IpcVizSink() = default;

IpcVizSink::~IpcVizSink() { stop(); }

bool IpcVizSink::start() {
    if (running_.exchange(true)) return true;
    acceptor_ = std::thread([this] { acceptorLoop(); });
    return true;
}

void IpcVizSink::stop() {
    if (!running_.exchange(false)) return;
    // Closing handles unblocks ConnectNamedPipe / WriteFile waits.
    {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        for (Client* c : clients_) {
            c->alive = false;
            c->cv.notify_all();
            if (c->handle) CloseHandle(c->handle);
        }
    }
    // Ping one instance to unblock the pending ConnectNamedPipe.
    HANDLE h = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (acceptor_.joinable()) acceptor_.join();
    std::lock_guard<std::mutex> lk(clientsMtx_);
    for (Client* c : clients_) {
        if (c->writer.joinable()) c->writer.join();
        delete c;
    }
    clients_.clear();
}

void IpcVizSink::acceptorLoop() {
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeA(
            kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        BOOL ok = ConnectNamedPipe(pipe, nullptr)
                  ? TRUE
                  : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!ok || !running_.load()) {
            CloseHandle(pipe);
            continue;
        }
        auto* c = new Client();
        c->handle = pipe;
        {
            std::lock_guard<std::mutex> lk(clientsMtx_);
            clients_.push_back(c);
        }
        c->writer = std::thread([this, c] { writerLoop(c); });
        printf("[ipc] client connected (%d subscriber(s))\n", clientCount());
    }
}

void IpcVizSink::writerLoop(Client* c) {
    while (c->alive.load()) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lk(c->mtx);
            c->cv.wait(lk, [c] { return !c->queue.empty() || !c->alive.load(); });
            if (!c->alive.load()) break;
            msg = std::move(c->queue.front());
            c->queue.pop_front();
        }
        DWORD written = 0;
        BOOL ok = WriteFile((HANDLE)c->handle, msg.data(), (DWORD)msg.size(),
                            &written, nullptr);
        if (!ok || written != msg.size()) {
            c->alive = false;
            break;
        }
    }
    // Cleanup this client.
    if (c->handle) CloseHandle((HANDLE)c->handle);
    c->handle = nullptr;
    std::lock_guard<std::mutex> lk(clientsMtx_);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if (*it == c) { clients_.erase(it); break; }
    }
    // This thread is c->writer itself: detach before freeing the struct.
    c->writer.detach();
    delete c;
}

void IpcVizSink::broadcast(std::string msg) {
    msg.push_back('\n');
    std::lock_guard<std::mutex> lk(clientsMtx_);
    for (Client* c : clients_) {
        if (!c->alive.load()) continue;
        {
            std::lock_guard<std::mutex> clk(c->mtx);
            if (c->queue.size() > 240) c->queue.clear();  // ~5s backlog → drop
            c->queue.push_back(msg);
        }
        c->cv.notify_one();
    }
}

std::string IpcVizSink::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

void IpcVizSink::onTrack(const TrackEvent& t) {
    std::ostringstream ss;
    ss << "{\"type\":\"track\",\"valid\":" << (t.valid ? "true" : "false")
       << ",\"title\":\"" << jsonEscape(t.title)
       << "\",\"artist\":\"" << jsonEscape(t.artist)
       << "\",\"album\":\"" << jsonEscape(t.album)
       << "\",\"cover_path\":\"" << jsonEscape(t.coverPath)
       << "\",\"confidence\":" << t.confidence
       << ",\"tentative\":" << (t.tentative ? "true" : "false") << "}";
    broadcast(ss.str());
}

void IpcVizSink::onSpectrum(const float* bins, int count, float peak) {
    std::ostringstream ss;
    ss << "{\"type\":\"spectrum\",\"bins\":[";
    for (int i = 0; i < count; ++i) {
        if (i) ss << ",";
        ss << bins[i];
    }
    ss << "],\"peak\":" << peak << "}";
    broadcast(ss.str());
}

void IpcVizSink::onStatus(VizStatus status) {
    const char* s = "standby";
    switch (status) {
        case VizStatus::Listening: s = "listening"; break;
        case VizStatus::Matching:  s = "matching";  break;
        case VizStatus::Mixing:    s = "mixing";    break;
        case VizStatus::Standby:   s = "standby";   break;
    }
    broadcast(std::string("{\"type\":\"status\",\"state\":\"") + s + "\"}");
}

int IpcVizSink::clientCount() {
    std::lock_guard<std::mutex> lk(clientsMtx_);
    int n = 0;
    for (Client* c : clients_) if (c->alive.load()) ++n;
    return n;
}

} // namespace vj
