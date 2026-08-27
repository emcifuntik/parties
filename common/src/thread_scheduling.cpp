#include <parties/thread_scheduling.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace parties {

TimerResolutionGuard::TimerResolutionGuard(unsigned int period_ms) noexcept
    : period_ms_(period_ms) {
#ifdef _WIN32
    active_ = period_ms_ != 0 && timeBeginPeriod(period_ms_) == TIMERR_NOERROR;
#else
    active_ = true;
#endif
}

TimerResolutionGuard::~TimerResolutionGuard() {
#ifdef _WIN32
    if (active_)
        timeEndPeriod(period_ms_);
#endif
}

bool set_current_thread_highest_priority() noexcept {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
#else
    return true;
#endif
}

} // namespace parties
