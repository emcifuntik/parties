#include <parties/video_send_controller.h>

#include <algorithm>
#include <cmath>

namespace parties {

namespace {

uint32_t scale_bitrate(uint32_t bps, double factor) {
    const double scaled = static_cast<double>(bps) * factor;
    if (scaled <= 0.0) return 0;
    const double max_u32 = 4294967295.0;
    if (scaled >= max_u32) return static_cast<uint32_t>(max_u32);
    return static_cast<uint32_t>(std::llround(scaled));
}

uint64_t counter_delta(uint64_t now, uint64_t before) {
    return now >= before ? now - before : 0;
}

}

uint32_t video_send_detail::queue_delay_ms(int64_t outstanding_bytes, uint32_t bitrate_bps,
                                           uint32_t rtt_us) {
    if (bitrate_bps == 0 || outstanding_bytes <= 0) return 0;
    const double drain_ms = static_cast<double>(outstanding_bytes) * 8.0 * 1000.0
                          / static_cast<double>(bitrate_bps);
    const double rtt_ms = static_cast<double>(rtt_us) / 1000.0;
    const double delay = drain_ms - rtt_ms;
    if (delay <= 0.0) return 0;
    const double max_u32 = 4294967295.0;
    return delay >= max_u32 ? static_cast<uint32_t>(max_u32) : static_cast<uint32_t>(delay);
}

void VideoSendAccounting::on_bytes_queued(size_t bytes) {
    c_.outstanding.fetch_add(static_cast<int64_t>(bytes));
}

void VideoSendAccounting::on_bytes_retired(size_t bytes) {
    const int64_t delta = static_cast<int64_t>(bytes);
    int64_t after = c_.outstanding.fetch_sub(delta) - delta;
    while (after < 0) {
        if (c_.outstanding.compare_exchange_weak(after, 0)) break;
    }
}

void VideoSendAccounting::on_send_failed() {
    c_.send_failures.fetch_add(1);
    c_.send_failures_window.fetch_add(1);
    c_.keyframe_request.store(true);
}

bool VideoSendAccounting::admit_frame() {
    if (c_.outstanding.load() <= c_.allowance_bytes.load()) return true;
    c_.admission_drops.fetch_add(1);
    c_.admission_drops_window.fetch_add(1);
    return false;
}

uint32_t VideoSendAccounting::bitrate_bps() const {
    return c_.bitrate_bps.load();
}

std::optional<uint32_t> VideoEncodeFeedback::take_bitrate_update() {
    const uint32_t v = c_.pending_bitrate.exchange(0);
    if (v == 0) return std::nullopt;
    return v;
}

bool VideoEncodeFeedback::take_keyframe_request() {
    return c_.keyframe_request.exchange(false);
}

VideoSendController::VideoSendController(const VideoSendControllerConfig& cfg) {
    start_share(cfg);
}

void VideoSendController::on_connection_lost() {
    start_share(cfg_);
    counters_.outstanding.store(0);
}

void VideoSendController::start_share(const VideoSendControllerConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.ceiling_bitrate_bps == 0) cfg_.ceiling_bitrate_bps = 1;
    if (cfg_.window_ms == 0) cfg_.window_ms = 1;
    if (cfg_.congested_windows == 0) cfg_.congested_windows = 1;
    if (cfg_.clean_windows == 0) cfg_.clean_windows = 1;

    counters_.admission_drops.store(0);
    counters_.admission_drops_window.store(0);
    counters_.send_failures.store(0);
    counters_.send_failures_window.store(0);
    counters_.keyframe_request.store(false);
    counters_.bitrate_bps.store(cfg_.ceiling_bitrate_bps);
    counters_.pending_bitrate.store(0);

    rtt_us_ = cfg_.default_rtt_us;
    window_start_us_.reset();
    congested_windows_ = 0;
    clean_windows_ = 0;
    last_queue_delay_ms_ = 0;
    peak_queue_delay_ms_ = 0;
    last_loss_ratio_ = 0.0;
    last_counters_.reset();
    window_lost_ = 0;
    window_sent_ = 0;
    published_bitrate_ = 0;
    decreases_ = 0;

    refresh_allowance();
    publish_bitrate(std::nullopt);
}

void VideoSendController::set_ceiling_bitrate(uint32_t bps) {
    if (bps == 0) return;
    cfg_.ceiling_bitrate_bps = bps;
    if (counters_.bitrate_bps.load() > bps) {
        counters_.bitrate_bps.store(bps);
        refresh_allowance();
        publish_bitrate(last_publish_us_);
    }
}

int64_t VideoSendController::outstanding_bytes() const {
    const int64_t v = counters_.outstanding.load();
    return v < 0 ? 0 : v;
}

void VideoSendController::on_path_counters(const QuicPathCounters& counters) {
    if (last_counters_) {
        const uint64_t d_suspected = counter_delta(counters.cumulative_suspected_lost_packets,
                                                   last_counters_->cumulative_suspected_lost_packets);
        const uint64_t d_spurious = counter_delta(counters.cumulative_spurious_lost_packets,
                                                  last_counters_->cumulative_spurious_lost_packets);
        window_lost_ += d_suspected > d_spurious ? d_suspected - d_spurious : 0;
        window_sent_ += counter_delta(counters.cumulative_sent_packets,
                                      last_counters_->cumulative_sent_packets);
    }
    last_counters_ = counters;
    if (counters.rtt_us != 0) rtt_us_ = counters.rtt_us;
    refresh_allowance();
}

uint32_t VideoSendController::floor_bps() const {
    return std::min(cfg_.floor_bitrate_bps, cfg_.ceiling_bitrate_bps);
}

void VideoSendController::tick(int64_t now_us) {
    if (!window_start_us_) window_start_us_ = now_us;

    last_queue_delay_ms_ = video_send_detail::queue_delay_ms(
        outstanding_bytes(), counters_.bitrate_bps.load(), rtt_us_);
    peak_queue_delay_ms_ = std::max(peak_queue_delay_ms_, last_queue_delay_ms_);

    const int64_t window_us = static_cast<int64_t>(cfg_.window_ms) * 1000;
    if (now_us - *window_start_us_ >= window_us) {
        last_loss_ratio_ = window_sent_ > 0
            ? static_cast<double>(window_lost_) / static_cast<double>(window_sent_)
            : 0.0;
        const uint32_t admission_drops = counters_.admission_drops_window.exchange(0);
        const uint32_t send_failures = counters_.send_failures_window.exchange(0);

        const bool congested =
            peak_queue_delay_ms_ >= cfg_.congestion_queue_ms ||
            last_loss_ratio_ > cfg_.loss_threshold ||
            admission_drops > 0 ||
            send_failures > 0;

        const uint32_t current = counters_.bitrate_bps.load();

        if (congested) {
            clean_windows_ = 0;
            if (++congested_windows_ >= cfg_.congested_windows) {
                congested_windows_ = 0;
                const uint32_t reduced = std::max(floor_bps(), scale_bitrate(current, cfg_.decrease_factor));
                if (reduced < current) {
                    counters_.bitrate_bps.store(reduced);
                    ++decreases_;
                }
            }
        } else {
            congested_windows_ = 0;
            if (++clean_windows_ >= cfg_.clean_windows) {
                clean_windows_ = 0;
                uint32_t raised = scale_bitrate(current, cfg_.increase_factor);
                if (raised <= current && current < cfg_.ceiling_bitrate_bps) raised = current + 1;
                raised = std::min(raised, cfg_.ceiling_bitrate_bps);
                if (raised > current) counters_.bitrate_bps.store(raised);
            }
        }

        window_start_us_ = now_us;
        peak_queue_delay_ms_ = 0;
        window_lost_ = 0;
        window_sent_ = 0;
    }

    refresh_allowance();
    publish_bitrate_if_due(now_us);
}

VideoSendController::Stats VideoSendController::stats() const {
    Stats s;
    s.bitrate_bps       = counters_.bitrate_bps.load();
    s.outstanding_bytes = outstanding_bytes();
    s.rtt_us            = rtt_us_;
    s.queue_delay_ms    = last_queue_delay_ms_;
    s.loss_ratio        = last_loss_ratio_;
    s.admission_drops   = counters_.admission_drops.load();
    s.send_failures     = counters_.send_failures.load();
    s.decreases         = decreases_;
    return s;
}

void VideoSendController::refresh_allowance() {
    const int64_t bitrate = counters_.bitrate_bps.load();
    const int64_t horizon_us = static_cast<int64_t>(rtt_us_)
                             + static_cast<int64_t>(cfg_.queue_budget_ms) * 1000;
    counters_.allowance_bytes.store(bitrate * horizon_us / 8 / 1'000'000);
}

void VideoSendController::publish_bitrate(std::optional<int64_t> at_us) {
    published_bitrate_ = counters_.bitrate_bps.load();
    last_publish_us_ = at_us;
    counters_.pending_bitrate.store(published_bitrate_);
}

void VideoSendController::publish_bitrate_if_due(int64_t now_us) {
    const uint32_t current = counters_.bitrate_bps.load();
    if (current == published_bitrate_) return;
    const bool at_bound = current == cfg_.ceiling_bitrate_bps || current == floor_bps();
    if (published_bitrate_ != 0 && !at_bound) {
        const double ratio = std::fabs(static_cast<double>(current) - static_cast<double>(published_bitrate_))
                           / static_cast<double>(published_bitrate_);
        if (ratio < cfg_.min_reconfigure_ratio) return;
    }
    if (last_publish_us_ &&
        now_us - *last_publish_us_ < static_cast<int64_t>(cfg_.min_reconfigure_ms) * 1000)
        return;
    publish_bitrate(now_us);
}

}
