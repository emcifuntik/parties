#pragma once

#include <chrono>
#include <optional>

namespace parties::client {

enum class SleepInhibitAction {
    None,
    Acquire,
    Release,
};

// A power transition (suspend/resume, session switch, display topology change)
// can drop the inhibit without notifying the process, so a held inhibit is
// re-asserted on a slow cadence.
class IdleSleepInhibitor {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds refresh_interval{30'000};

    SleepInhibitAction update(bool wanted, Clock::time_point now) {
        if (!wanted) {
            if (!next_refresh_)
                return SleepInhibitAction::None;
            next_refresh_.reset();
            return SleepInhibitAction::Release;
        }

        if (next_refresh_ && now < *next_refresh_)
            return SleepInhibitAction::None;

        next_refresh_ = now + refresh_interval;
        return SleepInhibitAction::Acquire;
    }

    bool held() const noexcept { return next_refresh_.has_value(); }

private:
    std::optional<Clock::time_point> next_refresh_;
};

}
