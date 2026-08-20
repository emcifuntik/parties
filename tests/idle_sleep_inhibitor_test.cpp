#include <client/idle_sleep_inhibitor.h>

#include <cstdint>
#include <iostream>

using parties::client::IdleSleepInhibitor;
using parties::client::SleepInhibitAction;

namespace {

const char* name(SleepInhibitAction action) {
    switch (action) {
    case SleepInhibitAction::None:    return "None";
    case SleepInhibitAction::Acquire: return "Acquire";
    case SleepInhibitAction::Release: return "Release";
    }
    return "?";
}

bool expect(SleepInhibitAction actual, SleepInhibitAction expected, const char* label) {
    if (actual == expected) return true;
    std::cerr << label << ": expected " << name(expected)
              << ", got " << name(actual) << "\n";
    return false;
}

constexpr int64_t kRefresh = IdleSleepInhibitor::refresh_interval_ms;

} // namespace

int main() {
    // Outside a call nothing is ever asserted, so the platform is never touched
    // and the machine keeps its normal idle behaviour.
    {
        IdleSleepInhibitor inhibitor;
        for (int64_t t = 0; t < 5 * kRefresh; t += 1000) {
            if (!expect(inhibitor.update(false, t), SleepInhibitAction::None,
                        "idle outside a call")) return 1;
        }
        if (inhibitor.held()) {
            std::cerr << "inhibit reported as held without a call\n";
            return 1;
        }
    }

    // Joining a channel asserts once; the ticks that follow are silent until the
    // refresh falls due. A 60 fps client ticks ~1800 times per refresh interval,
    // so anything else would be a per-frame syscall.
    {
        IdleSleepInhibitor inhibitor;
        if (!expect(inhibitor.update(true, 0), SleepInhibitAction::Acquire,
                    "join")) return 1;
        if (!inhibitor.held()) {
            std::cerr << "inhibit not held after joining\n";
            return 1;
        }
        for (int64_t t = 1; t < kRefresh; t += 16) {
            if (!expect(inhibitor.update(true, t), SleepInhibitAction::None,
                        "steady call")) return 1;
        }
        if (!expect(inhibitor.update(true, kRefresh), SleepInhibitAction::Acquire,
                    "refresh due")) return 1;
        if (!expect(inhibitor.update(true, kRefresh + 1), SleepInhibitAction::None,
                    "immediately after refresh")) return 1;
    }

    // A late tick refreshes from when it actually ran, not from when the refresh
    // was scheduled — a stalled frame must not make the next one land early.
    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, 0);
        if (!expect(inhibitor.update(true, kRefresh + 5), SleepInhibitAction::Acquire,
                    "late refresh")) return 1;
        if (!expect(inhibitor.update(true, 2 * kRefresh + 4), SleepInhibitAction::None,
                    "before the rescheduled refresh")) return 1;
        if (!expect(inhibitor.update(true, 2 * kRefresh + 5), SleepInhibitAction::Acquire,
                    "rescheduled refresh")) return 1;
    }

    // Leaving releases exactly once, so the display idle timers restart and the
    // release is not repeated on every subsequent tick.
    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, 0);
        if (!expect(inhibitor.update(false, 100), SleepInhibitAction::Release,
                    "leave")) return 1;
        if (inhibitor.held()) {
            std::cerr << "inhibit still held after leaving\n";
            return 1;
        }
        for (int64_t t = 101; t < kRefresh; t += 16) {
            if (!expect(inhibitor.update(false, t), SleepInhibitAction::None,
                        "after leaving")) return 1;
        }
    }

    // Rejoining inside the old refresh window must re-assert immediately: the
    // previous request is gone, and waiting out the remainder of the interval
    // would leave the display unprotected at the start of the new call.
    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, 0);
        inhibitor.update(false, 100);
        if (!expect(inhibitor.update(true, 150), SleepInhibitAction::Acquire,
                    "rejoin")) return 1;
        if (!expect(inhibitor.update(true, 200), SleepInhibitAction::None,
                    "steady after rejoin")) return 1;
        if (!expect(inhibitor.update(true, 150 + kRefresh), SleepInhibitAction::Acquire,
                    "refresh after rejoin")) return 1;
    }

    // steady_clock's epoch is arbitrary — on Windows it is the time since boot,
    // which is already large. The schedule must depend on elapsed time only.
    {
        IdleSleepInhibitor inhibitor;
        const int64_t boot = 4'000'000'000LL;   // ~46 days of uptime, in ms
        if (!expect(inhibitor.update(true, boot), SleepInhibitAction::Acquire,
                    "join at a large epoch")) return 1;
        if (!expect(inhibitor.update(true, boot + kRefresh - 1), SleepInhibitAction::None,
                    "steady at a large epoch")) return 1;
        if (!expect(inhibitor.update(true, boot + kRefresh), SleepInhibitAction::Acquire,
                    "refresh at a large epoch")) return 1;
    }

    std::cout << "Idle sleep inhibit policy passed\n";
    return 0;
}
