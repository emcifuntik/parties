#include <parties/video_backlog.h>

#include <cstdint>
#include <iostream>

using parties::Coalescer;
using parties::GateDecision;
using parties::MinRtt;
using parties::RateEstimator;
using parties::TokenBucket;
using parties::ViewerBacklogInputs;
using parties::ViewerVideoGate;
using parties::viewer_backlog_threshold_bytes;
using parties::VIDEO_BACKLOG_MAX_THRESHOLD_BYTES;
using parties::VIDEO_BACKLOG_MIN_THRESHOLD_BYTES;

namespace {

constexpr int64_t T0 = 3'000'000;
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

constexpr int64_t threshold(double rate, uint32_t min_rtt_us, int64_t headroom) {
    return viewer_backlog_threshold_bytes(
        ViewerBacklogInputs{rate, MinRtt::from_quic_min_rtt_us(min_rtt_us), headroom});
}

bool test_threshold_rate_only() {
    static_assert(VIDEO_BACKLOG_MIN_THRESHOLD_BYTES == 64 * 1024, "floor is 64 KB");
    static_assert(VIDEO_BACKLOG_MAX_THRESHOLD_BYTES == 2 * 1024 * 1024, "ceiling is 2 MB");
    if (threshold(0.0, 0, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: zero rate must clamp to the minimum");
    if (threshold(-1.0, 0, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: negative rate must clamp to the minimum");
    if (threshold(100'000.0, 0, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("threshold: 100 KB/s (25 KB queue) must clamp to the minimum");
    if (threshold(1'000'000.0, 0, 0) != 250'000)
        return fail("threshold: 1 MB/s must allow 250 ms = 250 KB");
    if (threshold(6'000'000.0, 0, 0) != 1'500'000)
        return fail("threshold: 6 MB/s must allow 1.5 MB (ceiling is 2 MB, not 1 MB)");
    if (threshold(10'000'000.0, 0, 0) != 2 * 1024 * 1024)
        return fail("threshold: 10 MB/s must clamp to 2 MB");
    if (threshold(100'000'000.0, 0, 0) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("threshold: 100 MB/s must clamp to the maximum");
    static_assert(threshold(1'000'000.0, 0, 0) == 250'000,
                  "threshold must be usable in constant expressions");
    return true;
}

bool test_threshold_rtt() {
    if (threshold(250'000.0, 230'000, 0) != 120'000)
        return fail("rtt threshold: 250 KB/s at 230 ms must be 120000, not the floor");
    static_assert(threshold(250'000.0, 230'000, 0) == 120'000,
                  "rtt threshold must be usable in constant expressions");

    {
        const int64_t v = threshold(3 * 250'000.0, 90'000, 0);
        if (v < 254'999 || v > 255'000)
            return fail("rtt threshold: 3 x 250 KB/s at 90 ms must be ~255000");
    }
    if (threshold(3 * 250'000.0, 150'000, 0) != 300'000)
        return fail("rtt threshold: 3 x 250 KB/s at 150 ms must be 300000");

    if (threshold(250'000.0, 230'000, 300'000) != 420'000)
        return fail("rtt threshold: keyframe headroom must be added");
    if (threshold(250'000.0, 230'000, -1) != 120'000)
        return fail("rtt threshold: negative headroom must be ignored");

    if (threshold(250'000.0, 1'000'000, 0) != 312'500)
        return fail("rtt threshold: 250 KB/s at 1 s must be 312500");
    if (threshold(250'000.0, 5'000'000, 0) != 312'500)
        return fail("rtt threshold: a 5 s MinRtt must clamp to 1 s");
    if (threshold(250'000.0, 0xFFFFFFFFu, 0) != 312'500)
        return fail("rtt threshold: UINT32_MAX MinRtt must clamp to 1 s");

    if (threshold(10'000'000.0, 1'000'000, 0) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("rtt threshold: 10 MB/s at 1 s must clamp to 2 MB");
    if (threshold(0.0, 0, 5'000'000) != VIDEO_BACKLOG_MAX_THRESHOLD_BYTES)
        return fail("rtt threshold: 5 MB headroom must clamp to 2 MB");

    if (threshold(100'000.0, 100'000, 0) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("rtt threshold: 100 KB/s at 100 ms (35 KB) must clamp to the minimum");
    if (threshold(0.0, 0, 1000) != VIDEO_BACKLOG_MIN_THRESHOLD_BYTES)
        return fail("rtt threshold: 1 KB headroom alone must clamp to the minimum");

    if (viewer_backlog_threshold_bytes(ViewerBacklogInputs{1'000'000.0}) != 300'000)
        return fail("rtt threshold: unmeasured MinRtt must assume 50 ms");
    return true;
}

bool test_viewer_gate() {
    ViewerVideoGate gate;
    if (gate.admit(0, 100, false) != GateDecision::Forward) return fail("gate: idle viewer must forward");
    if (gate.admit(100, 100, false) != GateDecision::Forward) return fail("gate: exactly at threshold must forward");
    if (gate.admit(101, 100, true) != GateDecision::Drop) return fail("gate: over threshold must drop a keyframe too");
    if (!gate.awaiting_keyframe()) return fail("gate: drop must arm awaiting_keyframe");
    if (gate.admit(50, 100, false) != GateDecision::DropAndRequestKeyframe)
        return fail("gate: awaiting delta must be dropped with a keyframe request");
    if (!gate.awaiting_keyframe()) return fail("gate: still awaiting after skipped delta");
    if (gate.admit(150, 100, false) != GateDecision::Drop) return fail("gate: over threshold while awaiting");
    if (gate.admit(50, 100, true) != GateDecision::Forward) return fail("gate: keyframe must resume delivery");
    if (gate.awaiting_keyframe()) return fail("gate: keyframe must clear awaiting_keyframe");
    if (gate.admit(50, 100, false) != GateDecision::Forward) return fail("gate: delta after keyframe must forward");
    gate.require_keyframe();
    if (gate.admit(0, 100, false) != GateDecision::DropAndRequestKeyframe)
        return fail("gate: require_keyframe must hold back deltas");
    if (gate.admit(0, 100, true) != GateDecision::Forward) return fail("gate: keyframe after require_keyframe");
    return true;
}

bool test_token_bucket() {
    TokenBucket bucket(4.0, 4.0);
    int64_t t = T0;
    for (int i = 0; i < 4; ++i)
        if (!bucket.try_take(t)) return fail("bucket: burst must allow four takes");
    if (bucket.try_take(t)) return fail("bucket: fifth take must be refused");
    t += 250 * MS;
    if (!bucket.try_take(t)) return fail("bucket: one token after 250 ms");
    if (bucket.try_take(t)) return fail("bucket: only one token after 250 ms");
    t += 10'000 * MS;
    for (int i = 0; i < 4; ++i)
        if (!bucket.try_take(t)) return fail("bucket: refill to burst");
    if (bucket.try_take(t)) return fail("bucket: refill must not exceed the burst");
    if (bucket.try_take(t - 1000 * MS)) return fail("bucket: backwards clock refilled");
    TokenBucket big(1.0, 3.0);
    if (!big.try_take(T0, 2.0)) return fail("bucket: cost 2 from 3 tokens");
    if (big.try_take(T0, 2.0)) return fail("bucket: cost 2 from 1 token");
    if (!big.try_take(T0, 1.0)) return fail("bucket: cost 1 from 1 token");
    return true;
}

bool test_token_bucket_at_time_zero() {
    TokenBucket bucket(4.0, 4.0);
    for (int i = 0; i < 4; ++i)
        if (!bucket.try_take(0)) return fail("bucket t0=0: burst must allow four takes");
    if (bucket.try_take(0)) return fail("bucket t0=0: fifth take must be refused");
    if (!bucket.try_take(250 * MS)) return fail("bucket t0=0: refill clock did not start at zero");
    return true;
}

bool test_rate_estimator() {
    RateEstimator est(1.0);
    if (est.bytes_per_second()) return fail("rate: estimate before any sample");
    est.add(T0, 1000);
    est.add(T0 + 500 * MS, 1000);
    if (est.bytes_per_second()) return fail("rate: estimate inside the first window");
    est.add(T0 + 1000 * MS, 1000);
    if (est.bytes_per_second() != 3000.0) return fail("rate: first window average");
    est.add(T0 + 2000 * MS, 1000);
    if (est.bytes_per_second() != 2000.0) return fail("rate: EWMA of the second window");
    est.add(T0 + 2500 * MS, 100'000);
    if (est.bytes_per_second() != 2000.0) return fail("rate: partial window changed the estimate");
    est.add(T0 + 4000 * MS, 0);
    if (est.bytes_per_second() != 0.5 * 2000.0 + 0.5 * 50'000.0) return fail("rate: long window");
    return true;
}

bool test_rate_estimator_at_time_zero() {
    RateEstimator est(1.0);
    est.add(0, 1000);
    est.add(500 * MS, 1000);
    est.add(1000 * MS, 1000);
    if (est.bytes_per_second() != 3000.0) return fail("rate t0=0: window restarted mid-way");
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

bool test_coalescer_at_time_zero() {
    Coalescer c(200 * MS);
    if (!c.allow(0)) return fail("coalescer t0=0: first event must pass");
    if (c.allow(100 * MS)) return fail("coalescer t0=0: event at zero did not start the interval");
    return true;
}

}

int main() {
    if (!test_threshold_rate_only()) return 1;
    if (!test_threshold_rtt()) return 1;
    if (!test_viewer_gate()) return 1;
    if (!test_token_bucket()) return 1;
    if (!test_token_bucket_at_time_zero()) return 1;
    if (!test_rate_estimator()) return 1;
    if (!test_rate_estimator_at_time_zero()) return 1;
    if (!test_coalescer()) return 1;
    if (!test_coalescer_at_time_zero()) return 1;

    std::cout << "Video backlog policy helpers passed\n";
    return 0;
}
