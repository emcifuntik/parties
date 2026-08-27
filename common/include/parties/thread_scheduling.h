#pragma once

namespace parties {

// Requests a process-local Windows timer resolution for this object's lifetime.
// On other platforms the request is a successful no-op.
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

// Uses the highest dynamic Windows thread priority without entering the
// realtime priority class. On other platforms this is a successful no-op.
bool set_current_thread_highest_priority() noexcept;

} // namespace parties
