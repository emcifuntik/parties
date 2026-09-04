#include <parties/video_send_controller.h>

#include <algorithm>
#include <cmath>

// Sharer-side congestion control. See the header for the threading contract:
// everything below the "Lock-free face" comment in the header is touched with
// atomics only; the AIMD/window state is owned by the main thread and mutated
// solely by tick() (plus the main-thread-only reset / set_target_bitrate /
// on_connection_stats entry points).
//
// This translation unit is also compiled stand-alone by
// tests/parties_video_send_controller_test, so it must not depend on the
// logging or tracing facilities of parties_common.

namespace parties {

namespace {

constexpr int64_t NO_WINDOW = -1;   // window_start_us_ sentinel: no window open yet

uint32_t scale_bitrate(uint32_t bps, double factor) {
    const double scaled = static_cast<double>(bps) * factor;
    if (scaled <= 0.0) return 0;
    const double max_u32 = 4294967295.0;
    if (scaled >= max_u32) return static_cast<uint32_t>(max_u32);
    return static_cast<uint32_t>(std::llround(scaled));
}

} // namespace

VideoSendController::VideoSendController()
    : VideoSendController(VideoSendControllerConfig{}) {}

VideoSendController::VideoSendController(const VideoSendControllerConfig& cfg) {
    reset(cfg);
}

void VideoSendController::reset() {
    reset(cfg_);
}

void VideoSendController::reset(const VideoSendControllerConfig& cfg) {
    cfg_ = cfg;
    // A ceiling of zero makes no sense (queue math divides by it); keep the
    // controller usable by forcing at least one bit per second.
    if (cfg_.target_bitrate_bps == 0) cfg_.target_bitrate_bps = 1;
    if (cfg_.window_ms == 0) cfg_.window_ms = 1;
    if (cfg_.congested_windows == 0) cfg_.congested_windows = 1;
    if (cfg_.clean_windows == 0) cfg_.clean_windows = 1;

    // Lock-free face. outstanding_ is deliberately left alone: sends of the
    // previous share may still be in flight on the same connection and their
    // SEND_COMPLETEs must keep matching what was added. clear_outstanding()
    // is the disconnect-time reset.
    admission_drops_.store(0);
    admission_drops_window_.store(0);
    send_failures_.store(0);
    encoder_drops_window_.store(0);
    keyframe_request_.store(false);
    bitrate_bps_.store(cfg_.target_bitrate_bps);
    pending_bitrate_.store(0);

    // Main-thread state
    rtt_us_ = cfg_.default_rtt_us;
    window_start_us_ = NO_WINDOW;
    congested_windows_ = 0;
    clean_windows_ = 0;
    last_queue_delay_ms_ = 0;
    peak_queue_delay_ms_ = 0;
    last_loss_ratio_ = 0.0;
    have_stats_ = false;
    last_suspected_lost_ = 0;
    last_spurious_lost_ = 0;
    last_sent_packets_ = 0;
    window_lost_ = 0;
    window_sent_ = 0;
    published_bitrate_ = 0;
    last_publish_us_ = 0;
    decreases_ = 0;

    refresh_allowance();

    // Publish the starting bitrate unconditionally so the encoder is configured
    // for the user's target as soon as the share starts: the first
    // take_bitrate_update() after a reset always returns cfg.target_bitrate_bps.
    // The publish is stamped at time 0, i.e. it never delays a real update.
    publish_bitrate_if_needed(0, true);
}

void VideoSendController::clear_outstanding() {
    // Only meaningful once the transport has completed or canceled every send
    // (connection gone): nothing will ever be subtracted for these bytes.
    outstanding_.store(0);
}

void VideoSendController::set_target_bitrate(uint32_t bps) {
    if (bps == 0) return;   // a zero ceiling is meaningless; ignore
    cfg_.target_bitrate_bps = bps;
    const uint32_t current = bitrate_bps_.load();
    if (current > bps) {
        // Lowering the ceiling takes effect immediately: the encoder must not
        // keep producing more than the user asked for.
        bitrate_bps_.store(bps);
        refresh_allowance();
        // Forced publish; keep the rate-limit timestamp of the last periodic
        // publish (this one is user-initiated and does not gate later updates).
        publish_bitrate_if_needed(last_publish_us_, true);
    }
    // Raising the ceiling only widens the room for future AIMD increases.
}

// ── Lock-free accounting ───────────────────────────────────────────────────

void VideoSendController::on_enqueued(size_t bytes) {
    outstanding_.fetch_add(static_cast<int64_t>(bytes));
}

void VideoSendController::on_completed(size_t bytes) {
    const int64_t delta = static_cast<int64_t>(bytes);
    int64_t after = outstanding_.fetch_sub(delta) - delta;
    // A completion for bytes that were never counted (e.g. a send issued
    // before reset()) must not leave the counter negative, otherwise later
    // sends would be under-counted. Clamp back to zero.
    while (after < 0) {
        if (outstanding_.compare_exchange_weak(after, 0)) break;
        // `after` now holds the freshly loaded value; loop while still negative.
    }
}

void VideoSendController::on_send_failed(size_t /*bytes*/) {
    // Nothing was queued, so outstanding_ is untouched. The frame is lost for
    // every viewer, so the next encoded frame has to be a keyframe.
    send_failures_.fetch_add(1);
    // Window flag: a send failure marks the current AIMD window as congested.
    // It shares the encoder-drop window counter (both mean "a frame was lost
    // on the sender before reaching the network").
    encoder_drops_window_.fetch_add(1);
    keyframe_request_.store(true);
}

void VideoSendController::on_encoder_drop() {
    encoder_drops_window_.fetch_add(1);
}

int64_t VideoSendController::outstanding_bytes() const {
    const int64_t v = outstanding_.load();
    return v < 0 ? 0 : v;
}

bool VideoSendController::admit_frame(int64_t /*now_us*/) {
    if (outstanding_.load() <= allowance_bytes_.load()) return true;
    admission_drops_.fetch_add(1);
    admission_drops_window_.fetch_add(1);
    return false;
}

// ── Main thread ────────────────────────────────────────────────────────────

void VideoSendController::on_connection_stats(int64_t /*now_us*/, uint32_t rtt_us,
                                              uint64_t suspected_lost_packets,
                                              uint64_t spurious_lost_packets,
                                              uint64_t sent_packets) {
    if (have_stats_) {
        // Absolute counters: only the deltas since the previous sample matter.
        // Guard against a counter that went backwards (should not happen with
        // MsQuic, but never let it poison the window).
        const uint64_t d_suspected = suspected_lost_packets >= last_suspected_lost_
            ? suspected_lost_packets - last_suspected_lost_ : 0;
        const uint64_t d_spurious = spurious_lost_packets >= last_spurious_lost_
            ? spurious_lost_packets - last_spurious_lost_ : 0;
        const uint64_t d_sent = sent_packets >= last_sent_packets_
            ? sent_packets - last_sent_packets_ : 0;
        // Spurious losses are suspected losses that turned out to be acked
        // after all: subtract them so only real loss counts.
        window_lost_ += d_suspected > d_spurious ? d_suspected - d_spurious : 0;
        window_sent_ += d_sent;
    }
    have_stats_ = true;
    last_suspected_lost_ = suspected_lost_packets;
    last_spurious_lost_ = spurious_lost_packets;
    last_sent_packets_ = sent_packets;
    if (rtt_us != 0) rtt_us_ = rtt_us;
    refresh_allowance();
}

void VideoSendController::tick(int64_t now_us) {
    if (window_start_us_ == NO_WINDOW) window_start_us_ = now_us;

    // Sample the queueing delay on every tick and keep the window peak.
    last_queue_delay_ms_ = queue_delay_ms(outstanding_bytes(), bitrate_bps_.load(), rtt_us_);
    peak_queue_delay_ms_ = std::max(peak_queue_delay_ms_, last_queue_delay_ms_);

    const int64_t window_us = static_cast<int64_t>(cfg_.window_ms) * 1000;
    if (now_us - window_start_us_ >= window_us) {
        // Close the window and classify it.
        last_loss_ratio_ = window_sent_ > 0
            ? static_cast<double>(window_lost_) / static_cast<double>(window_sent_)
            : 0.0;
        const uint32_t admission_drops = admission_drops_window_.exchange(0);
        const uint32_t sender_drops = encoder_drops_window_.exchange(0);   // encoder drops + send failures

        const bool congested =
            peak_queue_delay_ms_ >= cfg_.congestion_queue_ms ||
            last_loss_ratio_ > cfg_.loss_threshold ||
            admission_drops > 0 ||
            sender_drops > 0;

        const uint32_t floor_bps = std::min(cfg_.min_bitrate_bps, cfg_.target_bitrate_bps);
        const uint32_t current = bitrate_bps_.load();

        if (congested) {
            clean_windows_ = 0;
            if (++congested_windows_ >= cfg_.congested_windows) {
                congested_windows_ = 0;
                // Multiplicative decrease, never below the adaptation floor.
                const uint32_t reduced = std::max(floor_bps, scale_bitrate(current, cfg_.decrease_factor));
                if (reduced < current) {
                    bitrate_bps_.store(reduced);
                    ++decreases_;
                }
            }
        } else {
            congested_windows_ = 0;
            if (++clean_windows_ >= cfg_.clean_windows) {
                clean_windows_ = 0;
                // Increase back toward the user's ceiling.
                uint32_t raised = scale_bitrate(current, cfg_.increase_factor);
                if (raised <= current && current < cfg_.target_bitrate_bps) raised = current + 1;
                raised = std::min(raised, cfg_.target_bitrate_bps);
                if (raised > current) bitrate_bps_.store(raised);
            }
        }

        // Start the next window.
        window_start_us_ = now_us;
        peak_queue_delay_ms_ = 0;
        window_lost_ = 0;
        window_sent_ = 0;
    }

    refresh_allowance();
    publish_bitrate_if_needed(now_us, false);
}

uint32_t VideoSendController::bitrate_bps() const {
    return bitrate_bps_.load();
}

// ── Encode thread ──────────────────────────────────────────────────────────

std::optional<uint32_t> VideoSendController::take_bitrate_update() {
    const uint32_t v = pending_bitrate_.exchange(0);
    if (v == 0) return std::nullopt;
    return v;
}

bool VideoSendController::take_keyframe_request() {
    return keyframe_request_.exchange(false);
}

VideoSendController::Stats VideoSendController::stats() const {
    Stats s;
    s.bitrate_bps       = bitrate_bps_.load();
    s.outstanding_bytes = outstanding_bytes();
    s.rtt_us            = rtt_us_;
    s.queue_delay_ms    = last_queue_delay_ms_;
    s.loss_ratio        = last_loss_ratio_;
    s.admission_drops   = admission_drops_.load();
    s.send_failures     = send_failures_.load();
    s.decreases         = decreases_;
    return s;
}

// ── Helpers ────────────────────────────────────────────────────────────────

uint32_t VideoSendController::queue_delay_ms(int64_t outstanding_bytes, uint32_t bitrate_bps,
                                             uint32_t rtt_us) {
    if (bitrate_bps == 0 || outstanding_bytes <= 0) return 0;
    // Time to drain `outstanding` at `bitrate`, minus the share that is merely
    // in flight (one RTT worth of data is expected to be unacknowledged).
    const double drain_ms = static_cast<double>(outstanding_bytes) * 8.0 * 1000.0
                          / static_cast<double>(bitrate_bps);
    const double rtt_ms = static_cast<double>(rtt_us) / 1000.0;
    const double delay = drain_ms - rtt_ms;
    if (delay <= 0.0) return 0;
    const double max_u32 = 4294967295.0;
    return delay >= max_u32 ? static_cast<uint32_t>(max_u32) : static_cast<uint32_t>(delay);
}

void VideoSendController::refresh_allowance() {
    // Bytes that may be queued/in flight before admission starts skipping:
    // one RTT worth (in flight) plus queue_budget_ms of queueing.
    const int64_t bitrate = bitrate_bps_.load();
    const int64_t horizon_us = static_cast<int64_t>(rtt_us_)
                             + static_cast<int64_t>(cfg_.queue_budget_ms) * 1000;
    // bitrate/8 bytes per second * horizon seconds
    const int64_t allowance = bitrate * horizon_us / 8 / 1'000'000;
    allowance_bytes_.store(allowance);
}

void VideoSendController::publish_bitrate_if_needed(int64_t now_us, bool force) {
    const uint32_t current = bitrate_bps_.load();
    if (!force) {
        if (current == published_bitrate_) return;
        // Delta gate: ignore changes smaller than min_reconfigure_delta —
        // unless the adapted bitrate sits on a bound it can no longer move
        // away from (the user's target or the adaptation floor). The last step
        // onto a bound is often small (1'866'240 -> 2'000'000 is +7 %), and
        // since nothing moves the bitrate any further, skipping it would
        // strand the encoder just below the target (or above the floor) for
        // the rest of the share.
        const uint32_t floor_bps = std::min(cfg_.min_bitrate_bps, cfg_.target_bitrate_bps);
        const bool at_bound = current == cfg_.target_bitrate_bps || current == floor_bps;
        if (published_bitrate_ != 0 && !at_bound) {
            const double delta = std::fabs(static_cast<double>(current) - static_cast<double>(published_bitrate_))
                               / static_cast<double>(published_bitrate_);
            if (delta < cfg_.min_reconfigure_delta) return;
        }
        // Rate gate: never reconfigure the encoder faster than min_reconfigure_ms.
        if (now_us - last_publish_us_ < static_cast<int64_t>(cfg_.min_reconfigure_ms) * 1000) return;
    }
    published_bitrate_ = current;
    last_publish_us_ = now_us;
    pending_bitrate_.store(current);
}

} // namespace parties
