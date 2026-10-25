#pragma once

namespace parties {

class TimerResolutionGuard {
public:
    explicit TimerResolutionGuard(unsigned int period_ms) noexcept;
    ~TimerResolutionGuard();

    TimerResolutionGuard(const TimerResolutionGuard&) = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;

    bool active() const noexcept { return active_; }

private:
    unsigned int period_ms_ = 0;
    bool active_ = false;
};

bool set_current_thread_highest_priority() noexcept;

}
