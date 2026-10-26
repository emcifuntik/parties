#include <parties/thread_scheduling.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace parties {

WindowsTimerResolution::WindowsTimerResolution(unsigned int period_ms) noexcept
    : period_ms_(period_ms) {
#ifdef _WIN32
    active_ = period_ms_ != 0 && timeBeginPeriod(period_ms_) == TIMERR_NOERROR;
#endif
}

WindowsTimerResolution::~WindowsTimerResolution() {
#ifdef _WIN32
    if (active_)
        timeEndPeriod(period_ms_);
#endif
}

ThreadPriorityChange raise_thread_to_highest_normal_priority() noexcept {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE
        ? ThreadPriorityChange::Raised
        : ThreadPriorityChange::Failed;
#else
    return ThreadPriorityChange::Unsupported;
#endif
}

}
