// Offline, deterministic tests for the header-only server video backlog policy
// helpers in parties/video_backlog.h.

#include <parties/video_backlog.h>

#include <cstdint>
#include <iostream>

using parties::Coalescer;
using parties::RateEstimator;
using parties::TokenBucket;
using parties::ViewerVideoGate;
using parties::video_backlog_threshold_bytes;
using parties::VIDEO_BACKLOG_MAX_THRESHOLD_BYTES;
using parties::VIDEO_BACKLOG_MIN_THRESHOLD_BYTES;

namespace {

constexpr int64_t T0 = 3'000'000;
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

// Rate-only form (zero-RTT link): 250 ms of the sharer's rate, clamped.
bool test_threshold() {
    static_assert(VIDEO_BACKLOG_MIN_THRESHOLD_BYTES == 64 * 1024, "floor is 64 KB");
    static_assert(VIDEO_BACKLOG_MAX_THRESHOLD_BYTES == 2 * 1024 * 1024, "ceiling is 2 MB");
    if (video_backlog_threshold_bytes(0.0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: zero rate must clamp to the minimum");
    if (video_backlog_threshold_bytes(-1.0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: negative rate must clamp to the minimum");
    if (video_backlog_threshold_bytes(100'000.0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: 100 KB/s (25 KB queue) must clamp to the minimum");
    if (video_backlog_threshold_bytes(1'000'000.0) != 250'000)
        return fail("threshold: 1 MB/s must allow 250 ms = 250 KB");
    // 6 MB/s x 0.25 s = 1.5 MB: above the former 1 MB ceiling, below the 2 MB one.
    if (video_backlog_threshold_bytes(6'000'000.0) != 1'500'000)
        return fail("threshold: 6 MB/s must allow 1.5 MB (ceiling is 2 MB, not 1 MB)");
    // 10 MB/s x 0.25 s = 2.5 MB: clamped to the 2 MB ceiling.
    if (video_backlog_threshold_bytes(10'000'000.0) != 2 * 1024 * 1024)
        return fail("threshold: 10 MB/s must clamp to 2 MB");
    if (video_backlog_threshold_bytes(100'000'000.0) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("threshold: 100 MB/s must clamp to the maximum");
    static_assert(video_backlog_threshold_bytes(1'000'000.0) == 250'000,
                  "threshold must be usable in constant expressions");
    return true;
}

// Full form: viewer_rate (the SUM over every sharer the viewer watches) x
// (MinRtt + 250 ms) + keyframe headroom, clamped to [64 KB, 2 MB].
bool test_threshold_rtt() {
    // 250 KB/s at 230 ms MinRtt: 250000 x (0.25 + 0.23) = 120000. The rate-only
    // form would give 62500 -> the 64 KB floor, i.e. a long-RTT viewer on a
    // lossless link would be throttled by its own in-flight bytes.
    if (video_backlog_threshold_bytes(250'000.0, 230'000, 0) != 120'000)
        return fail("rtt threshold: 250 KB/s at 230 ms must be 120000, not the floor");
    static_assert(video_backlog_threshold_bytes(250'000.0, 230'000, 0) == 120'000,
                  "rtt threshold must be usable in constant expressions");

    // Multi-sharer: the caller passes the sum of the sharers' rates.
    // 3 x 250 KB/s at 90 ms: 750000 x 0.34 = 255000. The 0.34 s window is not
    // exactly representable (0.25 + 0.09 rounds one ulp below 0.34), so the
    // truncated product may land on 254999; either is the intended budget.
    {
        const int64_t v = video_backlog_threshold_bytes(3 * 250'000.0, 90'000, 0);
        if (v < 254'999 || v > 255'000)
            return fail("rtt threshold: 3 x 250 KB/s at 90 ms must be ~255000");
    }
    // Same sum at an exactly representable window: 750000 x 0.4 = 300000.
    if (video_backlog_threshold_bytes(3 * 250'000.0, 150'000, 0) != 300'000)
        return fail("rtt threshold: 3 x 250 KB/s at 150 ms must be 300000");

    // Keyframe headroom is added on top of the rate window; negative is ignored.
    if (video_backlog_threshold_bytes(250'000.0, 230'000, 300'000) != 420'000)
        return fail("rtt threshold: keyframe headroom must be added");
    if (video_backlog_threshold_bytes(250'000.0, 230'000, -1) != 120'000)
        return fail("rtt threshold: negative headroom must be ignored");

    // MinRtt is clamped at 1 s: 250000 x (0.25 + 1.0) = 312500.
    if (video_backlog_threshold_bytes(250'000.0, 1'000'000, 0) != 312'500)
        return fail("rtt threshold: 250 KB/s at 1 s must be 312500");
    if (video_backlog_threshold_bytes(250'000.0, 5'000'000, 0) != 312'500)
        return fail("rtt threshold: a 5 s MinRtt must clamp to 1 s");
    if (video_backlog_threshold_bytes(250'000.0, 0xFFFFFFFFu, 0) != 312'500)
        return fail("rtt threshold: UINT32_MAX MinRtt must clamp to 1 s");

    // Ceiling: 10 MB/s at 1 s = 12.5 MB -> 2 MB; headroom alone can hit it too.
    if (video_backlog_threshold_bytes(10'000'000.0, 1'000'000, 0) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("rtt threshold: 10 MB/s at 1 s must clamp to 2 MB");
    if (video_backlog_threshold_bytes(0.0, 0, 5'000'000) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("rtt threshold: 5 MB headroom must clamp to 2 MB");

    // Floor: nothing, tiny rates and tiny headroom all clamp up to 64 KB.
    if (video_backlog_threshold_bytes(0.0, 0, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("rtt threshold: zero must clamp to the minimum");
    if (video_backlog_threshold_bytes(100'000.0, 100'000, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("rtt threshold: 100 KB/s at 100 ms (35 KB) must clamp to the minimum");
    if (video_backlog_threshold_bytes(0.0, 0, 1000) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("rtt threshold: 1 KB headroom alone must clamp to the minimum");

    // Zero RTT and no headroom reduce to the rate-only form.
    if (video_backlog_threshold_bytes(1'000'000.0, 0, 0) != video_backlog_threshold_bytes(1'000'000.0))
        return fail("rtt threshold: zero RTT must equal the rate-only form");
    return true;
}

bool test_viewer_gate() {
    ViewerVideoGate gate;
    bool needs = true;
    if (!gate.admit(0, 100, false, &needs) || needs) return fail("gate: idle viewer must forward");
    if (!gate.admit(100, 100, false, &needs) || needs) return fail("gate: exactly at threshold must forward");
    // Over threshold: even a keyframe is dropped, and the viewer now waits.
    if (gate.admit(101, 100, true, &needs)) return fail("gate: over threshold must drop a keyframe too");
    if (needs) return fail("gate: dropping must not request a keyframe by itself");
    if (!gate.awaiting_keyframe) return fail("gate: drop must arm awaiting_keyframe");
    // Back under threshold: delta frames are useless and a keyframe is requested.
    if (gate.admit(50, 100, false, &needs)) return fail("gate: awaiting must skip delta frames");
    if (!needs) return fail("gate: awaiting delta must set needs_keyframe");
    if (!gate.awaiting_keyframe) return fail("gate: still awaiting after skipped delta");
    // Still over threshold while awaiting: dropped without a request.
    if (gate.admit(150, 100, false, &needs) || needs) return fail("gate: over threshold while awaiting");
    // Keyframe under threshold clears the wait.
    if (!gate.admit(50, 100, true, &needs) || needs) return fail("gate: keyframe must resume delivery");
    if (gate.awaiting_keyframe) return fail("gate: keyframe must clear awaiting_keyframe");
    if (!gate.admit(50, 100, false, &needs) || needs) return fail("gate: delta after keyframe must forward");
    // Null needs_keyframe pointer is allowed.
    if (gate.admit(200, 100, false, nullptr)) return fail("gate: null needs_keyframe drop");
    if (gate.admit(0, 100, false, nullptr)) return fail("gate: null needs_keyframe awaiting");
    if (!gate.admit(0, 100, true, nullptr)) return fail("gate: null needs_keyframe keyframe");
    return true;
}

bool test_token_bucket() {
    TokenBucket bucket(4.0, 4.0);
    int64_t t = T0;
    for (int i = 0; i < 4; ++i)
        if (!bucket.try_take(t)) return fail("bucket: burst must allow four takes");
    if (bucket.try_take(t)) return fail("bucket: fifth take must be refused");
    t += 250 * MS;   // one token refilled
    if (!bucket.try_take(t)) return fail("bucket: one token after 250 ms");
    if (bucket.try_take(t)) return fail("bucket: only one token after 250 ms");
    t += 10'000 * MS;   // refill saturates at the burst
    for (int i = 0; i < 4; ++i)
        if (!bucket.try_take(t)) return fail("bucket: refill to burst");
    if (bucket.try_take(t)) return fail("bucket: refill must not exceed the burst");
    // Time going backwards never refills.
    if (bucket.try_take(t - 1000 * MS)) return fail("bucket: backwards clock refilled");
    // Cost larger than one.
    TokenBucket big(1.0, 3.0);
    if (!big.try_take(T0, 2.0)) return fail("bucket: cost 2 from 3 tokens");
    if (big.try_take(T0, 2.0)) return fail("bucket: cost 2 from 1 token");
    if (!big.try_take(T0, 1.0)) return fail("bucket: cost 1 from 1 token");
    // Default-constructed bucket: one token per second, burst one.
    TokenBucket def;
    if (!def.try_take(T0)) return fail("bucket: default first take");
    if (def.try_take(T0 + 500 * MS)) return fail("bucket: default refill too fast");
    if (!def.try_take(T0 + 1500 * MS)) return fail("bucket: default refill after one second");
    return true;
}

bool test_rate_estimator() {
    RateEstimator est(1.0);
    if (est.ready()) return fail("rate: ready before any sample");
    if (est.bytes_per_second(123.0) != 123.0) return fail("rate: fallback before a full window");
    est.add(T0, 1000);
    est.add(T0 + 500 * MS, 1000);
    if (est.ready()) return fail("rate: ready before the window elapsed");
    if (est.bytes_per_second(123.0) != 123.0) return fail("rate: fallback inside the first window");
    est.add(T0 + 1000 * MS, 1000);
    if (!est.ready()) return fail("rate: not ready after a full window");
    if (est.bytes_per_second(123.0) != 3000.0) return fail("rate: first window average");
    // Second window: 1000 bytes in one second -> EWMA (3000 + 1000) / 2.
    est.add(T0 + 2000 * MS, 1000);
    if (est.bytes_per_second(123.0) != 2000.0) return fail("rate: EWMA of the second window");
    // Partial window does not change the estimate yet.
    est.add(T0 + 2500 * MS, 100'000);
    if (est.bytes_per_second(123.0) != 2000.0) return fail("rate: partial window changed the estimate");
    // A long quiet period then a sample: 100'000 + 0 over 2 s = 50'000 B/s instantaneous.
    est.add(T0 + 4000 * MS, 0);
    if (est.bytes_per_second(123.0) != 0.5 * 2000.0 + 0.5 * 50'000.0) return fail("rate: long window");
    return true;
}

bool test_coalescer() {
    Coalescer c(200 * MS);
    if (!c.allow(T0)) return fail("coalescer: first event must pass");
    if (c.allow(T0 + 100 * MS)) return fail("coalescer: event inside the interval must be suppressed");
    if (c.allow(T0 + 199 * MS)) return fail("coalescer: event just inside the interval");
    if (!c.allow(T0 + 200 * MS)) return fail("coalescer: event at the interval must pass");
    if (c.allow(T0 + 300 * MS)) return fail("coalescer: interval restarts at the allowed event");
    if (!c.allow(T0 + 400 * MS)) return fail("coalescer: next interval");
    return true;
}

} // namespace

int main() {
    if (!test_threshold()) return 1;
    if (!test_threshold_rtt()) return 1;
    if (!test_viewer_gate()) return 1;
    if (!test_token_bucket()) return 1;
    if (!test_rate_estimator()) return 1;
    if (!test_coalescer()) return 1;

    std::cout << "Video backlog policy helpers passed\n";
    return 0;
}
