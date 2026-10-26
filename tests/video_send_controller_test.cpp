#include <parties/video_send_controller.h>

#include <cstdint>
#include <iostream>
#include <optional>

using parties::VideoSendController;
using parties::VideoSendControllerConfig;
using parties::video_send_detail::queue_delay_ms;

namespace {

constexpr int64_t T0 = 10'000'000;
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

bool expect_update(VideoSendController& c, uint32_t expected, const char* label) {
    const auto v = c.encode_feedback().take_bitrate_update();
    if (!v || *v != expected) {
        std::cerr << label << ": expected bitrate update " << expected << ", got "
                  << (v ? static_cast<long long>(*v) : -1LL) << "\n";
        return false;
    }
    return true;
}

bool expect_no_update(VideoSendController& c, const char* label) {
    const auto v = c.encode_feedback().take_bitrate_update();
    if (v) {
        std::cerr << label << ": unexpected bitrate update " << *v << "\n";
        return false;
    }
    return true;
}

VideoSendControllerConfig default_cfg() {
    VideoSendControllerConfig cfg;
    cfg.ceiling_bitrate_bps = 2'000'000;
    cfg.floor_bitrate_bps = 800'000;
    cfg.default_rtt_us = 50'000;
    return cfg;
}

int64_t run_windows(VideoSendController& c, const VideoSendControllerConfig& cfg,
                    int64_t t, int count) {
    for (int i = 0; i < count; ++i) {
        t += static_cast<int64_t>(cfg.window_ms) * MS;
        c.tick(t);
    }
    return t;
}

bool test_queue_delay_helper() {
    if (queue_delay_ms(100'000, 4'000'000, 50'000) != 150)
        return fail("queue_delay_ms: 100 KB at 4 Mbps with 50 ms RTT should be 150 ms");
    if (queue_delay_ms(0, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: nothing outstanding should be 0");
    if (queue_delay_ms(100'000, 0, 50'000) != 0)
        return fail("queue_delay_ms: zero bitrate should be 0");
    if (queue_delay_ms(10'000, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: less than one RTT in flight should be 0");
    if (queue_delay_ms(-5, 4'000'000, 50'000) != 0)
        return fail("queue_delay_ms: negative outstanding should be 0");
    return true;
}

bool test_admission() {
    VideoSendController c(default_cfg());
    if (!expect_update(c, 2'000'000, "initial publish")) return false;
    if (!c.accounting().admit_frame()) return fail("admission: empty queue must admit");
    c.accounting().on_bytes_queued(42'500);
    if (c.outstanding_bytes() != 42'500) return fail("admission: outstanding accounting");
    if (!c.accounting().admit_frame()) return fail("admission: exactly at the allowance must admit");
    c.accounting().on_bytes_queued(1);
    if (c.accounting().admit_frame()) return fail("admission: above the allowance must skip");
    if (c.stats().admission_drops != 1) return fail("admission: drop not counted");
    c.accounting().on_bytes_retired(1);
    if (!c.accounting().admit_frame()) return fail("admission: back at the allowance must admit");
    if (c.stats().admission_drops != 1) return fail("admission: admitted frame counted as drop");
    c.accounting().on_bytes_retired(1'000'000);
    if (c.outstanding_bytes() != 0) return fail("admission: outstanding went negative");
    c.accounting().on_bytes_queued(10);
    if (c.outstanding_bytes() != 10) return fail("admission: counter drifted after clamp");

    c.accounting().on_bytes_retired(10);
    c.on_path_counters({200'000, 0, 0, 0});
    c.accounting().on_bytes_queued(80'000);
    if (!c.accounting().admit_frame()) return fail("admission: allowance did not follow the RTT");
    c.accounting().on_bytes_queued(1);
    if (c.accounting().admit_frame()) return fail("admission: allowance too wide after RTT update");
    return true;
}

bool test_decrease_and_floor() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("decrease: one congested window must not decrease");
    if (!expect_no_update(c, "decrease: no publish after one window")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("decrease: two congested windows must apply 0.8x");
    if (c.stats().decreases != 1) return fail("decrease: decreases counter");
    if (!expect_update(c, 1'600'000, "decrease: publish")) return false;
    t = run_windows(c, cfg, t, 2);
    if (c.accounting().bitrate_bps() != 1'280'000) return fail("decrease: second step");
    t = run_windows(c, cfg, t, 2);
    if (c.accounting().bitrate_bps() != 1'024'000) return fail("decrease: third step");
    t = run_windows(c, cfg, t, 2);
    if (c.accounting().bitrate_bps() != 819'200) return fail("decrease: fourth step");
    t = run_windows(c, cfg, t, 2);
    if (c.accounting().bitrate_bps() != 800'000) return fail("decrease: floor clamp");
    if (c.stats().decreases != 5) return fail("decrease: decreases counter at floor");
    t = run_windows(c, cfg, t, 10);
    if (c.accounting().bitrate_bps() != 800'000) return fail("decrease: went below the floor");
    if (c.stats().decreases != 5) return fail("decrease: counted decreases that did nothing");
    if (c.stats().queue_delay_ms == 0) return fail("decrease: stats queue delay not sampled");
    if (!expect_update(c, 800'000, "decrease: floor must be published despite the delta gate")) return false;
    return true;
}

bool test_increase_and_ceiling() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, T0, 2);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("increase: setup decrease");
    (void)c.encode_feedback().take_bitrate_update();
    c.accounting().on_bytes_retired(200'000);
    t = run_windows(c, cfg, t, 3);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("increase: raised before four clean windows");
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'728'000) return fail("increase: four clean windows must apply 1.08x");
    t = run_windows(c, cfg, t, 40);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("increase: ceiling clamp");
    t = run_windows(c, cfg, t, 8);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("increase: went above the ceiling");
    return true;
}

bool test_clean_window_resets_congestion_count() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    c.accounting().on_bytes_retired(200'000);
    t = run_windows(c, cfg, t, 1);
    c.accounting().on_bytes_queued(200'000);
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("streak: non-consecutive congested windows decreased");
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("streak: consecutive congested windows did not decrease");
    return true;
}

bool test_loss_driven_decrease() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    c.on_path_counters({50'000, 0, 0, 0});
    c.on_path_counters({50'000, 10, 0, 100});
    int64_t t = run_windows(c, cfg, T0, 1);
    if (c.stats().loss_ratio < 0.099 || c.stats().loss_ratio > 0.101)
        return fail("loss: window loss ratio");
    c.on_path_counters({50'000, 20, 10, 200});
    t = run_windows(c, cfg, t, 1);
    if (c.stats().loss_ratio != 0.0) return fail("loss: spurious losses were counted as loss");
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("loss: decreased on a clean window");
    c.on_path_counters({50'000, 30, 10, 300});
    t = run_windows(c, cfg, t, 1);
    c.on_path_counters({50'000, 40, 10, 400});
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("loss: two lossy windows must decrease");
    if (c.stats().rtt_us != 50'000) return fail("loss: rtt not stored");
    c.on_path_counters({50'000, 42, 10, 500});
    t = run_windows(c, cfg, t, 1);
    c.on_path_counters({50'000, 44, 10, 600});
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("loss: below-threshold loss decreased");
    return true;
}

bool test_publish_rate_limit() {
    VideoSendControllerConfig cfg = default_cfg();
    cfg.window_ms = 100;
    cfg.congested_windows = 1;
    cfg.floor_bitrate_bps = 100'000;
    cfg.min_reconfigure_ms = 500;
    VideoSendController c(cfg);
    if (!expect_update(c, 2'000'000, "rate limit: initial publish")) return false;
    c.tick(T0);
    c.accounting().on_bytes_queued(1'000'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (!expect_update(c, 1'600'000, "rate limit: first decrease publishes")) return false;
    t = run_windows(c, cfg, t, 1);
    if (!expect_no_update(c, "rate limit: +100 ms")) return false;
    t = run_windows(c, cfg, t, 1);
    if (!expect_no_update(c, "rate limit: +200 ms")) return false;
    t = run_windows(c, cfg, t, 1);
    if (!expect_no_update(c, "rate limit: +300 ms")) return false;
    t = run_windows(c, cfg, t, 1);
    if (!expect_no_update(c, "rate limit: +400 ms")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 524'288) return fail("rate limit: bitrate progression");
    if (!expect_update(c, 524'288, "rate limit: publish after min_reconfigure_ms")) return false;
    return true;
}

bool test_publish_delta_gate() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, T0, 2);
    if (!expect_update(c, 1'600'000, "delta gate: decrease publishes")) return false;
    c.accounting().on_bytes_retired(200'000);
    t = run_windows(c, cfg, t, 4);
    if (c.accounting().bitrate_bps() != 1'728'000) return fail("delta gate: first increase");
    if (!expect_no_update(c, "delta gate: 8 % change must not publish")) return false;
    t = run_windows(c, cfg, t, 4);
    if (c.accounting().bitrate_bps() != 1'866'240) return fail("delta gate: second increase");
    if (!expect_update(c, 1'866'240, "delta gate: 16 % change publishes")) return false;
    return true;
}

bool test_publish_at_target_bypasses_delta_gate() {
    VideoSendControllerConfig cfg = default_cfg();
    cfg.window_ms = 250;
    cfg.congested_windows = 1;
    cfg.clean_windows = 1;
    cfg.min_reconfigure_ms = 500;
    VideoSendController c(cfg);
    if (!expect_update(c, 2'000'000, "at target: initial publish")) return false;
    c.tick(T0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, T0, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("at target: decrease");
    if (!expect_update(c, 1'600'000, "at target: decrease publishes")) return false;
    c.accounting().on_bytes_retired(200'000);
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'728'000) return fail("at target: first increase");
    if (!expect_no_update(c, "at target: 8 % step below the delta gate must not publish")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'866'240) return fail("at target: second increase");
    if (!expect_update(c, 1'866'240, "at target: 16 % step publishes")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("at target: ceiling reached");
    if (!expect_no_update(c, "at target: rate gate must still hold at the target")) return false;
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("at target: bitrate moved off the ceiling");
    if (!expect_update(c, 2'000'000, "at target: target must be published on the next tick after the rate gate")) return false;
    t = run_windows(c, cfg, t, 8);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("at target: went above the ceiling");
    if (!expect_no_update(c, "at target: nothing to publish once at the target")) return false;
    return true;
}

bool test_start_share_publishes_initial() {
    VideoSendController c(default_cfg());
    if (!expect_update(c, 2'000'000, "start_share: constructor publishes the target")) return false;
    if (!expect_no_update(c, "start_share: publish must be consumed once")) return false;
    c.accounting().on_bytes_queued(12345);
    c.accounting().on_send_failed();
    VideoSendControllerConfig cfg = default_cfg();
    cfg.ceiling_bitrate_bps = 1'500'000;
    c.start_share(cfg);
    if (!expect_update(c, 1'500'000, "start_share: publishes the new target")) return false;
    if (c.outstanding_bytes() != 12345) return fail("start_share: must keep the bytes of sends still in flight");
    if (c.encode_feedback().take_keyframe_request()) return fail("start_share: keyframe request not cleared");
    const auto s = c.stats();
    if (s.send_failures != 0 || s.admission_drops != 0 || s.decreases != 0 || s.bitrate_bps != 1'500'000)
        return fail("start_share: stats not cleared");
    return true;
}

bool test_start_share_keeps_outstanding() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    c.accounting().on_bytes_queued(1000);
    c.start_share(cfg);
    if (c.outstanding_bytes() != 1000) return fail("start_share: outstanding bytes must survive");
    c.accounting().on_bytes_retired(400);
    if (c.outstanding_bytes() != 600) return fail("start_share: completion after restart not matched");
    return true;
}

bool test_connection_lost_clears_outstanding() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.set_ceiling_bitrate(1'000'000);
    (void)c.encode_feedback().take_bitrate_update();
    c.accounting().on_bytes_queued(1000);
    c.accounting().on_send_failed();
    c.on_connection_lost();
    if (c.outstanding_bytes() != 0) return fail("connection lost: counter not zeroed");
    if (c.accounting().bitrate_bps() != 1'000'000) return fail("connection lost: ceiling not kept");
    if (!expect_update(c, 1'000'000, "connection lost: restart publish missing")) return false;
    if (c.encode_feedback().take_keyframe_request()) return fail("connection lost: keyframe request kept");
    if (c.stats().send_failures != 0) return fail("connection lost: stats not cleared");
    c.accounting().on_bytes_queued(10);
    if (c.outstanding_bytes() != 10) return fail("connection lost: counter drifted afterwards");
    return true;
}

bool test_set_target() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.set_ceiling_bitrate(1'000'000);
    if (c.accounting().bitrate_bps() != 1'000'000) return fail("set_target: lowering must clamp immediately");
    if (!expect_update(c, 1'000'000, "set_target: lowering forces a publish")) return false;
    c.set_ceiling_bitrate(3'000'000);
    if (c.accounting().bitrate_bps() != 1'000'000) return fail("set_target: raising must not change the bitrate");
    if (!expect_no_update(c, "set_target: raising must not publish")) return false;
    c.tick(T0);
    run_windows(c, cfg, T0, 4);
    if (c.accounting().bitrate_bps() != 1'080'000) return fail("set_target: increase after raising the ceiling");
    c.set_ceiling_bitrate(500'000);
    if (c.accounting().bitrate_bps() != 500'000) return fail("set_target: target below the floor");
    return true;
}

bool test_send_failure_keyframe_and_congestion() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    if (c.encode_feedback().take_keyframe_request()) return fail("send failure: spurious keyframe request");
    c.accounting().on_bytes_queued(1000);
    c.accounting().on_send_failed();
    if (c.outstanding_bytes() != 1000) return fail("send failure: must not touch outstanding bytes");
    if (!c.encode_feedback().take_keyframe_request()) return fail("send failure: keyframe request missing");
    if (c.encode_feedback().take_keyframe_request()) return fail("send failure: keyframe request must be one-shot");
    if (c.stats().send_failures != 1) return fail("send failure: counter");
    c.tick(T0);
    int64_t t = run_windows(c, cfg, T0, 1);
    c.accounting().on_send_failed();
    t = run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("send failure: two failing windows must decrease");
    return true;
}

bool test_admission_drops_congest() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(T0);
    int64_t t = T0;
    for (int window = 0; window < 2; ++window) {
        c.accounting().on_bytes_queued(1'000'000);
        if (c.accounting().admit_frame()) return fail("drops: admission should skip");
        c.accounting().on_bytes_retired(1'000'000);
        t = run_windows(c, cfg, t, 1);
    }
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("drops: two windows of admission drops must decrease");
    return true;
}

bool test_window_starts_at_time_zero() {
    const auto cfg = default_cfg();
    VideoSendController c(cfg);
    (void)c.encode_feedback().take_bitrate_update();
    c.tick(0);
    c.accounting().on_bytes_queued(200'000);
    int64_t t = run_windows(c, cfg, 0, 1);
    if (c.accounting().bitrate_bps() != 2'000'000) return fail("t0=0: decreased after one window");
    run_windows(c, cfg, t, 1);
    if (c.accounting().bitrate_bps() != 1'600'000) return fail("t0=0: window clock did not start at zero");
    return true;
}

bool test_first_publish_after_start_at_time_zero() {
    VideoSendControllerConfig cfg = default_cfg();
    cfg.window_ms = 100;
    cfg.congested_windows = 1;
    cfg.min_reconfigure_ms = 500;
    VideoSendController c(cfg);
    if (!expect_update(c, 2'000'000, "t0=0: initial publish")) return false;
    c.tick(0);
    c.accounting().on_bytes_queued(1'000'000);
    run_windows(c, cfg, 0, 1);
    if (!expect_update(c, 1'600'000, "t0=0: first adaptation must not wait for the rate gate")) return false;
    return true;
}

}

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
    if (!test_start_share_publishes_initial()) return 1;
    if (!test_start_share_keeps_outstanding()) return 1;
    if (!test_connection_lost_clears_outstanding()) return 1;
    if (!test_set_target()) return 1;
    if (!test_send_failure_keyframe_and_congestion()) return 1;
    if (!test_admission_drops_congest()) return 1;
    if (!test_window_starts_at_time_zero()) return 1;
    if (!test_first_publish_after_start_at_time_zero()) return 1;

    std::cout << "Video send controller admission and AIMD policy passed\n";
    return 0;
}
