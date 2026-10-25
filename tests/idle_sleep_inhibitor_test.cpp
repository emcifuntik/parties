#include <client/idle_sleep_inhibitor.h>

#include <chrono>
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

using std::chrono::milliseconds;

constexpr int64_t kRefresh = IdleSleepInhibitor::refresh_interval.count();

IdleSleepInhibitor::Clock::time_point at(int64_t ms) {
    return IdleSleepInhibitor::Clock::time_point{} + milliseconds(ms);
}

}

int main() {
    {
        IdleSleepInhibitor inhibitor;
        for (int64_t t = 0; t < 5 * kRefresh; t += 1000) {
            if (!expect(inhibitor.update(false, at(t)), SleepInhibitAction::None,
                        "idle outside a call")) return 1;
        }
        if (inhibitor.held()) {
            std::cerr << "inhibit reported as held without a call\n";
            return 1;
        }
    }

    {
        IdleSleepInhibitor inhibitor;
        if (!expect(inhibitor.update(true, at(0)), SleepInhibitAction::Acquire,
                    "join")) return 1;
        if (!inhibitor.held()) {
            std::cerr << "inhibit not held after joining\n";
            return 1;
        }
        for (int64_t t = 1; t < kRefresh; t += 16) {
            if (!expect(inhibitor.update(true, at(t)), SleepInhibitAction::None,
                        "steady call")) return 1;
        }
        if (!expect(inhibitor.update(true, at(kRefresh)), SleepInhibitAction::Acquire,
                    "refresh due")) return 1;
        if (!expect(inhibitor.update(true, at(kRefresh + 1)), SleepInhibitAction::None,
                    "immediately after refresh")) return 1;
    }

    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, at(0));
        if (!expect(inhibitor.update(true, at(kRefresh + 5)), SleepInhibitAction::Acquire,
                    "late refresh")) return 1;
        if (!expect(inhibitor.update(true, at(2 * kRefresh + 4)), SleepInhibitAction::None,
                    "before the rescheduled refresh")) return 1;
        if (!expect(inhibitor.update(true, at(2 * kRefresh + 5)), SleepInhibitAction::Acquire,
                    "rescheduled refresh")) return 1;
    }

    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, at(0));
        if (!expect(inhibitor.update(false, at(100)), SleepInhibitAction::Release,
                    "leave")) return 1;
        if (inhibitor.held()) {
            std::cerr << "inhibit still held after leaving\n";
            return 1;
        }
        for (int64_t t = 101; t < kRefresh; t += 16) {
            if (!expect(inhibitor.update(false, at(t)), SleepInhibitAction::None,
                        "after leaving")) return 1;
        }
    }

    {
        IdleSleepInhibitor inhibitor;
        inhibitor.update(true, at(0));
        inhibitor.update(false, at(100));
        if (!expect(inhibitor.update(true, at(150)), SleepInhibitAction::Acquire,
                    "rejoin")) return 1;
        if (!expect(inhibitor.update(true, at(200)), SleepInhibitAction::None,
                    "steady after rejoin")) return 1;
        if (!expect(inhibitor.update(true, at(150 + kRefresh)), SleepInhibitAction::Acquire,
                    "refresh after rejoin")) return 1;
    }

    {
        IdleSleepInhibitor inhibitor;
        const int64_t boot = 4'000'000'000LL;
        if (!expect(inhibitor.update(true, at(boot)), SleepInhibitAction::Acquire,
                    "join at a large epoch")) return 1;
        if (!expect(inhibitor.update(true, at(boot + kRefresh - 1)), SleepInhibitAction::None,
                    "steady at a large epoch")) return 1;
        if (!expect(inhibitor.update(true, at(boot + kRefresh)), SleepInhibitAction::Acquire,
                    "refresh at a large epoch")) return 1;
    }

    std::cout << "Idle sleep inhibit policy passed\n";
    return 0;
}
