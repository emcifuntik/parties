#pragma once

#include <cstdint>

namespace parties::client {

// What the owner should do with the platform sleep inhibit this tick.
enum class SleepInhibitAction {
    None,     // nothing changed — the common case
    Acquire,  // assert, or re-assert, the inhibit
    Release,  // drop it
};

// Decides when to hold the display/system awake while the user is in a call.
//
// A voice call produces no keyboard or mouse input, so the OS idle timers run
// to completion and the monitor powers off mid-conversation. Every platform can
// suppress that, but each exposes a different kind of handle: Windows holds a
// power request, macOS an activity token, iOS flips a flag on the application
// object. This class owns only the policy — when the inhibit is taken, refreshed
// and dropped — so the decision is testable without an OS, and the platform layer
// is left with a single set_keep_awake(bool) call.
//
// The refresh matters because the assertion is only as durable as the state
// carrying it: a power transition (suspend/resume, session switch, display
// topology change) can drop the request without notifying the process. Making
// the owner re-assert on a slow cadence restores it within one interval, and
// costs two syscalls a minute while a call is up. Re-asserting an inhibit that
// is still held is a no-op on every platform.
class IdleSleepInhibitor {
public:
    // Interval between re-assertions while the inhibit is held.
    static constexpr int64_t refresh_interval_ms = 30'000;

    // `wanted` is the desired state — true while connected to a voice channel.
    // `now_ms` must come from a monotonic clock. Call this every tick; it
    // returns None on all but the transition and refresh ticks.
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

} // namespace parties::client
