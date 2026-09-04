// Offline, deterministic tests for parties::VideoSendController (sharer-side
// admission + AIMD bitrate control). No transport, no clock: time is passed in.

#include <parties/video_send_controller.h>

#include <cstdint>
#include <iostream>
#include <optional>

using parties::VideoSendController;
using parties::VideoSendControllerConfig;

namespace {

constexpr int64_t T0 = 10'000'000;   // 10 s: mimics a monotonic clock that is not near zero
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

bool expect_update(VideoSendController& c, uint32_t expected, const char* label) {
    const auto v = c.take_bitrate_update();
    if (!v || *v != expected) {
        std::cerr << label << ": expected bitrate update " << expected << ", got "
                  << (v ? static_cast<long long>(*v) : -1LL) << "\n";
        return false;
    }
    return true;
}

bool expect_no_update(VideoSendController& c, const char* label) {
    const auto v = c.take_bitrate_update();
    if (v) {
        std::cerr << label << ": unexpected bitrate update " << *v << "\n";
        return false;
    }
    return true;
}

VideoSendControllerConfig default_cfg() {
    VideoSendControllerConfig cfg;
    cfg.target_bitrate_bps = 2'000'000;
    cfg.min_bitrate_bps = 800'000;
    cfg.default_rtt_us = 50'000;
    return cfg;
}

// Advance `count` windows of cfg.window_ms starting after `t`, returning the
// time of the last tick.
int64_t run_windows(VideoSendController& c, const VideoSendControllerConfig& cfg,
                    int64_t t, int count) {
    for (int i = 0; i < count; ++i) {
        t += static_cast<int64_t>(cfg.window_ms) * MS;
        c.tick(t);
    }
    return t;
}

bool test_queue_delay_helper() {
    if (VideoSendController::queue_delay_ms(100'000, 4'000'000, 50'000) != 150)
        return fail("queue_delay_ms: 100 KB at 4 Mbps with 50 ms RTT should be 150 ms");
    if (VideoSendController::queue_delay_ms(0, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: nothing outstanding should be 0");
    if (VideoSendController::queue_delay_ms(100'000, 0, 50'000) != 0)
        return fail("queue_delay_ms: zero bitrate should be 0");
    if (VideoSendController::queue_delay_ms(10'000, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: less than one RTT in flight should be 0");
    if (VideoSendController::queue_delay_ms(-5, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: negative outstanding should be 0");
    return true;
}

bool test_admission() {
    VideoSendController c(default_cfg());
    // allowance = 2 Mbps / 8 * (50 ms + 120 ms) = 42'500 bytes
    if (!expect_update(c, 2'000'000, "initial publish")) return false;
    if (!c.admit_frame(T0)) return fail("admission: empty queue must admit");
    c.on_enqueued(42'500);
    if (c.outstanding_bytes() != 42'500) return fail("admission: outstanding accounting");
    if (!c.admit_frame(T0)) return fail("admission: exactly at the allowance must admit");
    c.on_enqueued(1);
    if (c.admit_frame(T0)) return fail("admission: above the allowance must skip");
    if (c.stats().admission_drops != 1) return fail("admission: drop not counted");
    c.on_completed(1);
    if (!c.admit_frame(T0)) return fail("admission: back at the allowance must admit");
    if (c.stats().admission_drops != 1) return fail("admission: admitted frame counted as drop");
    // Over-completion never goes negative.
    c.on_completed(1'000'000);
    if (c.outstanding_bytes() != 0) return fail("admission: outstanding went negative");
    c.on_enqueued(10);
    if (c.outstanding_bytes() != 10) return fail("admission: counter drifted after clamp");

    // A larger RTT widens the allowance: 2 Mbps / 8 * (200 + 120) ms = 80'000.
    c.on_completed(10);
    c.on_connection_stats(T0, 200'000, 0, 0, 0);
    c.on_enqueued(80'000);
    if (!c.admit_frame(T0)) return fail("admission: allowance did not follow the RTT");
    c.on_enqueued(1);
    if (c.admit_frame(T0)) return fail("admission: allowance too wide after RTT update");
    return true;
}

bool test_decrease_and_floor() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    // 200 KB queued at 2 Mbps = 800 ms drain - 50 ms RTT = 750 ms of queueing.
    c.on_enqueued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (c.bitrate_bps() != 2'000'000) return fail("decrease: one congested window must not decrease");
    if (!expect_no_update(c, "decrease: no publish after one window")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'600'000) return fail("decrease: two congested windows must apply 0.8x");
    if (c.stats().decreases != 1) return fail("decrease: decreases counter");
    if (!expect_update(c, 1'600'000, "decrease: publish")) return false;
    // Keep it congested: 1.6M -> 1.28M -> 1.024M -> 819'200 -> 800'000 (floor).
    t = run_windows(c, cfg, t, 2);
    if (c.bitrate_bps() != 1'280'000) return fail("decrease: second step");
    t = run_windows(c, cfg, t, 2);
    if (c.bitrate_bps() != 1'024'000) return fail("decrease: third step");
    t = run_windows(c, cfg, t, 2);
    if (c.bitrate_bps() != 819'200) return fail("decrease: fourth step");
    t = run_windows(c, cfg, t, 2);
    if (c.bitrate_bps() != 800'000) return fail("decrease: floor clamp");
    if (c.stats().decreases != 5) return fail("decrease: decreases counter at floor");
    t = run_windows(c, cfg, t, 10);
    if (c.bitrate_bps() != 800'000) return fail("decrease: went below the floor");
    if (c.stats().decreases != 5) return fail("decrease: counted decreases that did nothing");
    if (c.stats().queue_delay_ms == 0) return fail("decrease: stats queue delay not sampled");
    // The last step onto the floor (819'200 -> 800'000, +2.3 %) is below the
    // delta gate but must still reach the encoder: the pending update holds
    // the floor, not the previous step.
    if (!expect_update(c, 800'000, "decrease: floor must be published despite the delta gate")) return false;
    return true;
}

bool test_increase_and_ceiling() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    c.on_enqueued(200'000);
    int64_t t = run_windows(c, cfg, T0, 2);
    if (c.bitrate_bps() != 1'600'000) return fail("increase: setup decrease");
    (void)c.take_bitrate_update();
    c.on_completed(200'000);
    // Three clean windows: nothing yet.
    t = run_windows(c, cfg, t, 3);
    if (c.bitrate_bps() != 1'600'000) return fail("increase: raised before four clean windows");
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'728'000) return fail("increase: four clean windows must apply 1.08x");
    // Enough clean windows to hit the ceiling.
    t = run_windows(c, cfg, t, 40);
    if (c.bitrate_bps() != 2'000'000) return fail("increase: ceiling clamp");
    t = run_windows(c, cfg, t, 8);
    if (c.bitrate_bps() != 2'000'000) return fail("increase: went above the ceiling");
    return true;
}

bool test_clean_window_resets_congestion_count() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    c.on_enqueued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);      // congested
    c.on_completed(200'000);
    t = run_windows(c, cfg, t, 1);               // clean: resets the streak
    c.on_enqueued(200'000);
    t = run_windows(c, cfg, t, 1);               // congested (streak = 1)
    if (c.bitrate_bps() != 2'000'000) return fail("streak: non-consecutive congested windows decreased");
    t = run_windows(c, cfg, t, 1);               // congested (streak = 2)
    if (c.bitrate_bps() != 1'600'000) return fail("streak: consecutive congested windows did not decrease");
    return true;
}

bool test_loss_driven_decrease() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    // First sample only establishes the baseline.
    c.on_connection_stats(T0, 50'000, 0, 0, 0);
    // Window 1: 10 % real loss.
    c.on_connection_stats(T0 + 250 * MS, 50'000, 10, 0, 100);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (c.stats().loss_ratio < 0.099 || c.stats().loss_ratio > 0.101)
        return fail("loss: window loss ratio");
    // Window 2: 10 more suspected, all of them spurious -> 0 % real loss -> clean.
    c.on_connection_stats(t + 250 * MS, 50'000, 20, 10, 200);
    t = run_windows(c, cfg, t, 1);
    if (c.stats().loss_ratio != 0.0) return fail("loss: spurious losses were counted as loss");
    if (c.bitrate_bps() != 2'000'000) return fail("loss: decreased on a clean window");
    // Windows 3 and 4: 10 % loss each -> decrease.
    c.on_connection_stats(t + 250 * MS, 50'000, 30, 10, 300);
    t = run_windows(c, cfg, t, 1);
    c.on_connection_stats(t + 250 * MS, 50'000, 40, 10, 400);
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'600'000) return fail("loss: two lossy windows must decrease");
    if (c.stats().rtt_us != 50'000) return fail("loss: rtt not stored");
    // Loss just under the threshold is clean: 2 lost / 100 sent.
    c.on_connection_stats(t + 250 * MS, 50'000, 42, 10, 500);
    t = run_windows(c, cfg, t, 1);
    c.on_connection_stats(t + 250 * MS, 50'000, 44, 10, 600);
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'600'000) return fail("loss: below-threshold loss decreased");
    return true;
}

bool test_publish_rate_limit() {
    VideoSendControllerConfig cfg = default_cfg();
    cfg.window_ms = 100;
    cfg.congested_windows = 1;
    cfg.min_bitrate_bps = 100'000;
    cfg.min_reconfigure_ms = 500;
    VideoSendController c(cfg);
    if (!expect_update(c, 2'000'000, "rate limit: initial publish")) return false;
    c.tick(T0);
    c.on_enqueued(1'000'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (!expect_update(c, 1'600'000, "rate limit: first decrease publishes")) return false;
    t = run_windows(c, cfg, t, 1);   // 1'280'000 at +200 ms: too soon
    if (!expect_no_update(c, "rate limit: +100 ms")) return false;
    t = run_windows(c, cfg, t, 1);   // 1'024'000 at +300 ms
    if (!expect_no_update(c, "rate limit: +200 ms")) return false;
    t = run_windows(c, cfg, t, 1);   // 819'200 at +400 ms
    if (!expect_no_update(c, "rate limit: +300 ms")) return false;
    t = run_windows(c, cfg, t, 1);   // 655'360 at +500 ms
    if (!expect_no_update(c, "rate limit: +400 ms")) return false;
    t = run_windows(c, cfg, t, 1);   // 524'288 at +600 ms: 500 ms since the last publish
    if (c.bitrate_bps() != 524'288) return fail("rate limit: bitrate progression");
    if (!expect_update(c, 524'288, "rate limit: publish after min_reconfigure_ms")) return false;
    return true;
}

bool test_publish_delta_gate() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    c.on_enqueued(200'000);
    int64_t t = run_windows(c, cfg, T0, 2);
    if (!expect_update(c, 1'600'000, "delta gate: decrease publishes")) return false;
    c.on_completed(200'000);
    t = run_windows(c, cfg, t, 4);   // 1'728'000: +8 % vs published, below the 10 % gate
    if (c.bitrate_bps() != 1'728'000) return fail("delta gate: first increase");
    if (!expect_no_update(c, "delta gate: 8 % change must not publish")) return false;
    t = run_windows(c, cfg, t, 4);   // 1'866'240: +16.6 % vs published
    if (c.bitrate_bps() != 1'866'240) return fail("delta gate: second increase");
    if (!expect_update(c, 1'866'240, "delta gate: 16 % change publishes")) return false;
    return true;
}

// The delta gate must not apply once the adapted bitrate sits on the target:
// the last step there (+7 %) would otherwise never be published and the
// encoder would stay below the target for the rest of the share. Windows of
// 250 ms with single-window AIMD reactions make the 500 ms publish rate gate
// visible in the sequence.
bool test_publish_at_target_bypasses_delta_gate() {
    VideoSendControllerConfig cfg = default_cfg();
    cfg.window_ms = 250;
    cfg.congested_windows = 1;
    cfg.clean_windows = 1;
    cfg.min_reconfigure_ms = 500;
    VideoSendController c(cfg);
    if (!expect_update(c, 2'000'000, "at target: initial publish")) return false;
    c.tick(T0);
    c.on_enqueued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);   // T0 + 250 ms: congested -> 1'600'000
    if (c.bitrate_bps() != 1'600'000) return fail("at target: decrease");
    if (!expect_update(c, 1'600'000, "at target: decrease publishes")) return false;
    c.on_completed(200'000);
    t = run_windows(c, cfg, t, 1);            // T0 + 500 ms: 1'728'000, +8 % -> delta gate
    if (c.bitrate_bps() != 1'728'000) return fail("at target: first increase");
    if (!expect_no_update(c, "at target: 8 % step below the delta gate must not publish")) return false;
    t = run_windows(c, cfg, t, 1);            // T0 + 750 ms: 1'866'240, +16.6 %, 500 ms since the last publish
    if (c.bitrate_bps() != 1'866'240) return fail("at target: second increase");
    if (!expect_update(c, 1'866'240, "at target: 16 % step publishes")) return false;
    t = run_windows(c, cfg, t, 1);            // T0 + 1000 ms: 2'000'000 (clamped), only 250 ms since the last publish
    if (c.bitrate_bps() != 2'000'000) return fail("at target: ceiling reached");
    if (!expect_no_update(c, "at target: rate gate must still hold at the target")) return false;
    t = run_windows(c, cfg, t, 1);            // T0 + 1250 ms: rate gate satisfied -> the target is published
    if (c.bitrate_bps() != 2'000'000) return fail("at target: bitrate moved off the ceiling");
    if (!expect_update(c, 2'000'000, "at target: target must be published on the next tick after the rate gate")) return false;
    t = run_windows(c, cfg, t, 8);
    if (c.bitrate_bps() != 2'000'000) return fail("at target: went above the ceiling");
    if (!expect_no_update(c, "at target: nothing to publish once at the target")) return false;
    return true;
}

bool test_reset_publishes_initial() {
    VideoSendController c(default_cfg());
    if (!expect_update(c, 2'000'000, "reset: constructor publishes the target")) return false;
    if (!expect_no_update(c, "reset: publish must be consumed once")) return false;
    c.on_enqueued(12345);
    c.on_send_failed(10);
    VideoSendControllerConfig cfg = default_cfg();
    cfg.target_bitrate_bps = 1'500'000;
    c.reset(cfg);
    if (!expect_update(c, 1'500'000, "reset: publishes the new target")) return false;
    if (c.outstanding_bytes() != 12345) return fail("reset: must keep the bytes of sends still in flight");
    if (c.take_keyframe_request()) return fail("reset: keyframe request not cleared");
    const auto s = c.stats();
    if (s.send_failures != 0 || s.admission_drops != 0 || s.decreases != 0 || s.bitrate_bps != 1'500'000)
        return fail("reset: stats not cleared");
    return true;
}

// A share restart must not forget bytes that are still in flight on the same
// connection (their SEND_COMPLETEs still arrive); only the disconnect-time
// clear_outstanding() zeroes the counter.
bool test_reset_keeps_outstanding() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    c.on_enqueued(1000);
    c.reset(cfg);
    if (c.outstanding_bytes() != 1000) return fail("reset(cfg): outstanding bytes must survive");
    c.reset();
    if (c.outstanding_bytes() != 1000) return fail("reset(): outstanding bytes must survive");
    // Completions of the previous share still settle against the counter.
    c.on_completed(400);
    if (c.outstanding_bytes() != 600) return fail("reset: completion after reset not matched");
    c.clear_outstanding();
    if (c.outstanding_bytes() != 0) return fail("clear_outstanding: counter not zeroed");
    // clear_outstanding touches nothing else: the pending publish from reset()
    // is still there and the adapted bitrate is unchanged.
    if (c.bitrate_bps() != 2'000'000) return fail("clear_outstanding: bitrate changed");
    if (!expect_update(c, 2'000'000, "clear_outstanding: pending publish lost")) return false;
    c.on_enqueued(10);
    if (c.outstanding_bytes() != 10) return fail("clear_outstanding: counter drifted afterwards");
    return true;
}

bool test_set_target() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.set_target_bitrate(1'000'000);
    if (c.bitrate_bps() != 1'000'000) return fail("set_target: lowering must clamp immediately");
    if (!expect_update(c, 1'000'000, "set_target: lowering forces a publish")) return false;
    c.set_target_bitrate(3'000'000);
    if (c.bitrate_bps() != 1'000'000) return fail("set_target: raising must not change the bitrate");
    if (!expect_no_update(c, "set_target: raising must not publish")) return false;
    // Clean windows now climb toward the new ceiling.
    c.tick(T0);
    run_windows(c, cfg, T0, 4);
    if (c.bitrate_bps() != 1'080'000) return fail("set_target: increase after raising the ceiling");
    // Lowering below the adaptation floor still clamps (the floor follows the target).
    c.set_target_bitrate(500'000);
    if (c.bitrate_bps() != 500'000) return fail("set_target: target below the floor");
    return true;
}

bool test_send_failure_keyframe_and_congestion() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    if (c.take_keyframe_request()) return fail("send failure: spurious keyframe request");
    c.on_enqueued(1000);
    c.on_send_failed(5000);
    if (c.outstanding_bytes() != 1000) return fail("send failure: must not touch outstanding bytes");
    if (!c.take_keyframe_request()) return fail("send failure: keyframe request missing");
    if (c.take_keyframe_request()) return fail("send failure: keyframe request must be one-shot");
    if (c.stats().send_failures != 1) return fail("send failure: counter");
    // A failure in each of two consecutive windows is congestion.
    c.tick(T0);
    int64_t t = run_windows(c, cfg, T0, 1);      // failure above landed in this window
    c.on_send_failed(5000);
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'600'000) return fail("send failure: two failing windows must decrease");
    return true;
}

bool test_encoder_and_admission_drops_congest() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.take_bitrate_update();
    c.tick(T0);
    c.on_encoder_drop();
    int64_t t = run_windows(c, cfg, T0, 1);
    // Admission drop in the second window.
    c.on_enqueued(1'000'000);
    if (c.admit_frame(t)) return fail("drops: admission should skip");
    c.on_completed(1'000'000);
    t = run_windows(c, cfg, t, 1);
    if (c.bitrate_bps() != 1'600'000) return fail("drops: encoder + admission drops must decrease");
    return true;
}

} // namespace

int main() {
    if (!test_queue_delay_helper()) return 1;
    if (!test_admission()) return 1;
    if (!test_decrease_and_floor()) return 1;
    if (!test_increase_and_ceiling()) return 1;
    if (!test_clean_window_resets_congestion_count()) return 1;
    if (!test_loss_driven_decrease()) return 1;
    if (!test_publish_rate_limit()) return 1;
    if (!test_publish_delta_gate()) return 1;
    if (!test_publish_at_target_bypasses_delta_gate()) return 1;
    if (!test_reset_publishes_initial()) return 1;
    if (!test_reset_keeps_outstanding()) return 1;
    if (!test_set_target()) return 1;
    if (!test_send_failure_keyframe_and_congestion()) return 1;
    if (!test_encoder_and_admission_drops_congest()) return 1;

    std::cout << "Video send controller admission and AIMD policy passed\n";
    return 0;
}
