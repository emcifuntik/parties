#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace parties::client {

// UI activity follows audible source PCM before receiver volume controls.
// A deadline prevents a stalled audio callback from leaving the indicator on.
class AudioActivity {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto hold = std::chrono::milliseconds(500);
    static constexpr float minimum_rms = 0.001f;

    void observe_level(float rms, Clock::time_point now = Clock::now()) {
        if (std::isfinite(rms) && rms > minimum_rms)
            active_until_.store(ticks(now + hold), std::memory_order_relaxed);
    }

    void observe_pcm(const float* pcm, size_t count) {
        if (!pcm || count == 0) return;
        float energy = 0.0f;
        for (size_t i = 0; i < count; ++i)
            energy += pcm[i] * pcm[i];
        observe_level(std::sqrt(energy / static_cast<float>(count)));
    }

    bool active(Clock::time_point now = Clock::now()) const {
        const auto until = active_until_.load(std::memory_order_relaxed);
        return until != 0 && ticks(now) < until;
    }

    void reset() { active_until_.store(0, std::memory_order_relaxed); }

private:
    static int64_t ticks(Clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            time.time_since_epoch()).count();
    }
    std::atomic<int64_t> active_until_{0};
};

} // namespace parties::client
