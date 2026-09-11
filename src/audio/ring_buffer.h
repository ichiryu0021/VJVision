// Thread-safe ring buffer for mono float samples (resampled to 44.1 kHz).
// The capture thread writes; the match thread reads the latest N seconds.
// Overwrites oldest data when full.
#pragma once
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace vj {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacitySamples)
        : buf_(capacitySamples), cap_(capacitySamples) {}

    void write(const float* data, size_t n) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < n; ++i) {
            buf_[head_] = data[i];
            head_ = (head_ + 1) % cap_;
            if (size_ < cap_) ++size_;
        }
    }

    // Returns the most recent `n` samples in chronological order.
    // If fewer than `n` samples are available, the result is zero-padded
    // at the front (keeps time alignment of the tail).
    std::vector<float> readLatest(size_t n) const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<float> out(n, 0.f);
        size_t take = (std::min)(n, size_);
        if (take == 0) return out;
        // Index of the oldest sample we want (last `take` samples).
        size_t start = (head_ + cap_ - take) % cap_;
        for (size_t i = 0; i < take; ++i)
            out[n - take + i] = buf_[(start + i) % cap_];
        return out;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return size_;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        head_ = 0;
        size_ = 0;
    }

private:
    mutable std::mutex mtx_;
    std::vector<float> buf_;
    size_t cap_;
    size_t head_ = 0;   // next write position
    size_t size_ = 0;   // valid samples
};

} // namespace vj
