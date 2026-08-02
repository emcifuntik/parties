#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace parties::encdec::nvidia::detail {

enum class SurfaceWaitResult {
    Available,
    Cancelled,
    TimedOut,
    InvalidSlot,
};

template <class Rep, class Period, class Predicate>
SurfaceWaitResult wait_for_surface_release(
        std::condition_variable_any& released,
        std::mutex& mutex,
        std::stop_token stop_token,
        const std::chrono::duration<Rep, Period>& timeout,
        Predicate available) {
    if (available()) return SurfaceWaitResult::Available;

    std::unique_lock lock(mutex);
    if (released.wait_for(lock, stop_token, timeout, available))
        return SurfaceWaitResult::Available;
    return stop_token.stop_requested()
        ? SurfaceWaitResult::Cancelled
        : SurfaceWaitResult::TimedOut;
}

} // namespace parties::encdec::nvidia::detail
