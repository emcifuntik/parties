#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace parties::client {

class FrameRequests {
public:
    FrameRequests() : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~FrameRequests() {
        if (wake_) CloseHandle(wake_);
    }
    FrameRequests(const FrameRequests&) = delete;
    FrameRequests& operator=(const FrameRequests&) = delete;

    void request() {
        if (!requested_.exchange(true, std::memory_order_release) && wake_) SetEvent(wake_);
    }
    bool take() { return requested_.exchange(false, std::memory_order_acquire); }
    void wait(uint32_t milliseconds) {
        if (wake_) WaitForSingleObject(wake_, milliseconds);
        else Sleep(milliseconds);
    }

private:
    std::atomic<bool> requested_{true};
    HANDLE wake_;
};

using RenderClock = std::chrono::steady_clock;

// RmlUi's GetNextUpdateDelay() is seconds from the last Update, or infinity.
inline RenderClock::time_point ui_update_deadline(RenderClock::time_point now, double delay_seconds) {
    if (!(delay_seconds < 3600.0)) return (RenderClock::time_point::max)();
    return now + std::chrono::duration_cast<RenderClock::duration>(
        std::chrono::duration<double>(delay_seconds));
}

inline std::optional<uint32_t> idle_wait_ms(bool dirty, RenderClock::time_point now,
                                            RenderClock::time_point deadline) {
    constexpr std::chrono::milliseconds kMaxIdle{1000};
    if (dirty || now >= deadline) return std::nullopt;
    if (deadline - now >= kMaxIdle) return static_cast<uint32_t>(kMaxIdle.count());
    return static_cast<uint32_t>(
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
}

}
