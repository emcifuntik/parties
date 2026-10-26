#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace parties::client {

class ServerQueryWakeEvent {
public:
    void request() {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            pending_ = true;
        }
        condition_.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        condition_.notify_all();
    }

    bool wait_for_next(std::chrono::milliseconds period) {
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, period, [this] { return pending_ || stopped_; });
        if (stopped_) return false;
        pending_ = false;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool pending_ = false;
    bool stopped_ = false;
};

}
