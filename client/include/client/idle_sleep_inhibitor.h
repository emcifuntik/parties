#pragma once

#include <cstdint>

namespace parties::client {

enum class SleepInhibitAction {
    None,
    Acquire,
    Release,
};

// The refresh matters because the assertion is only as durable as the state
// carrying it: a power transition (suspend/resume, session switch, display
// topology change) can drop the request without notifying the process. Making
// the owner re-assert on a slow cadence restores it within one interval, and
// costs two syscalls a minute while a call is up. Re-asserting an inhibit that
// is still held is a no-op on every platform.
class IdleSleepInhibitor {
public:
    static constexpr int64_t refresh_interval_ms = 30'000;

    SleepInhibitAction update(bool wanted, int64_t now_ms) {
        if (!wanted) {
            if (!held_)
                return SleepInhibitAction::None;
            held_ = false;
            return SleepInhibitAction::Release;
        }

        if (held_ && now_ms < next_refresh_ms_)
            return SleepInhibitAction::None;

        held_ = true;
        next_refresh_ms_ = now_ms + refresh_interval_ms;
        return SleepInhibitAction::Acquire;
    }

    bool held() const noexcept { return held_; }

private:
    bool    held_            = false;
    int64_t next_refresh_ms_ = 0;
};

}
