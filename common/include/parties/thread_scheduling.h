#pragma once

namespace parties {

class WindowsTimerResolution {
public:
    explicit WindowsTimerResolution(unsigned int period_ms) noexcept;
    ~WindowsTimerResolution();

    WindowsTimerResolution(const WindowsTimerResolution&) = delete;
    WindowsTimerResolution& operator=(const WindowsTimerResolution&) = delete;

    bool active() const noexcept { return active_; }

private:
    unsigned int period_ms_ = 0;
    bool active_ = false;
};

enum class ThreadPriorityChange { Raised, Failed, Unsupported };

[[nodiscard]] ThreadPriorityChange raise_thread_to_highest_normal_priority() noexcept;

}
