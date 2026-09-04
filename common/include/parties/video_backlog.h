#pragma once

// Pure policy helpers shared by the server (per-viewer video backlog) and the
// tests. No transport dependencies.
//
// Root cause of the "slow motion" bug: video was written to a reliable QUIC
// stream with no accounting, so when a link carried less than the encoder
// produced the queue grew without bound and every frame arrived late. The fix
// is to measure bytes queued + in flight per viewer (StreamSend minus
// SEND_COMPLETE, which fires on acknowledgement) and to drop WHOLE frames for
// that viewer once the backlog exceeds a rate-relative threshold. After a drop
// the viewer needs a keyframe before it can use anything else.
//
// Implementation: header-only. Tests: tests/video_backlog_test.cpp

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace parties {

// Bytes of video that may be queued/in flight toward one viewer before whole
// frames are dropped. The counter (StreamSend minus SEND_COMPLETE) also holds
// the bytes that are merely in flight — with SendBufferingEnabled=FALSE a send
// completes only when acknowledged, i.e. never before one RTT — so the budget
// must contain an RTT term or a long-RTT viewer is throttled on a lossless link.
// Budget = viewer_rate x (min_rtt + 250 ms) + keyframe headroom, where
// viewer_rate is the SUM of the rates of every sharer the viewer watches
// (they all share the one counter) and the headroom is the largest keyframe
// recently seen from those sharers (one admitted keyframe must not by itself
// trip the gate for the deltas behind it). Clamped so very low rates do not
// degenerate into seconds of queue and very high rates keep a bounded queue.
constexpr int64_t VIDEO_BACKLOG_MIN_THRESHOLD_BYTES = 64 * 1024;
constexpr int64_t VIDEO_BACKLOG_MAX_THRESHOLD_BYTES = 2 * 1024 * 1024;
constexpr double  VIDEO_BACKLOG_QUEUE_SECONDS       = 0.25;
constexpr uint32_t VIDEO_BACKLOG_DEFAULT_RTT_US     = 50'000;   // until the first sample
constexpr uint32_t VIDEO_BACKLOG_MAX_RTT_US         = 1'000'000;

// Rate-only form (no RTT term). Kept for callers/tests that model a
// zero-RTT link; the server uses the full form below.
constexpr int64_t video_backlog_threshold_bytes(double send_rate_bytes_per_s) {
    const double raw = send_rate_bytes_per_s * VIDEO_BACKLOG_QUEUE_SECONDS;
    const int64_t v = raw <= 0.0 ? 0 : static_cast<int64_t>(raw);
    return std::clamp<int64_t>(v, VIDEO_BACKLOG_MIN_THRESHOLD_BYTES,
                               VIDEO_BACKLOG_MAX_THRESHOLD_BYTES);
}

// Full form: viewer_rate_bytes_per_s = sum over subscribed sharers,
// min_rtt_us = the viewer connection's MinRtt (QUIC_STATISTICS_V2::MinRtt —
// NOT the smoothed Rtt, which already contains the queueing the gate is
// trying to detect), keyframe_headroom_bytes = largest recent keyframe.
constexpr int64_t video_backlog_threshold_bytes(double viewer_rate_bytes_per_s,
                                                uint32_t min_rtt_us,
                                                int64_t keyframe_headroom_bytes) {
    const uint32_t rtt = min_rtt_us > VIDEO_BACKLOG_MAX_RTT_US ? VIDEO_BACKLOG_MAX_RTT_US : min_rtt_us;
    const double window_s = VIDEO_BACKLOG_QUEUE_SECONDS + static_cast<double>(rtt) / 1e6;
    const double raw = viewer_rate_bytes_per_s * window_s;
    int64_t v = raw <= 0.0 ? 0 : static_cast<int64_t>(raw);
    if (keyframe_headroom_bytes > 0) v += keyframe_headroom_bytes;
    return std::clamp<int64_t>(v, VIDEO_BACKLOG_MIN_THRESHOLD_BYTES,
                               VIDEO_BACKLOG_MAX_THRESHOLD_BYTES);
}

// Per (viewer, sharer) forwarding gate. Mutated only under the owner's lock
// (server: Server::subscriptions_mutex_).
struct ViewerVideoGate {
    bool awaiting_keyframe = false;   // a frame was dropped; only a keyframe may resume delivery

    // Decide whether to forward one frame.
    //   outstanding  bytes queued + in flight toward this viewer
    //   threshold    video_backlog_threshold_bytes(...)
    //   keyframe     the frame's keyframe flag
    // Returns true to forward. Sets *needs_keyframe when the viewer is now
    // waiting for a keyframe that the sharer has not produced yet, i.e. the
    // caller should request one (coalesced per sharer).
    bool admit(int64_t outstanding, int64_t threshold, bool keyframe, bool* needs_keyframe) {
        if (needs_keyframe) *needs_keyframe = false;
        if (outstanding > threshold) {
            // Over budget: drop this frame whatever it is. Dropping a keyframe
            // is deliberate — queuing it would only add more delay.
            awaiting_keyframe = true;
            return false;
        }
        if (awaiting_keyframe) {
            if (!keyframe) {
                if (needs_keyframe) *needs_keyframe = true;
                return false;
            }
            awaiting_keyframe = false;
        }
        return true;
    }
};

// Simple token bucket (control-message rate limiting). Not thread-safe.
class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(double tokens_per_second, double burst)
        : rate_(tokens_per_second), burst_(burst), tokens_(burst) {}

    bool try_take(int64_t now_us, double cost = 1.0) {
        refill(now_us);
        if (tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

private:
    double  rate_  = 1.0;
    double  burst_ = 1.0;
    double  tokens_ = 1.0;
    int64_t last_us_ = 0;

    void refill(int64_t now_us) {
        if (last_us_ == 0) { last_us_ = now_us; return; }
        const double dt = static_cast<double>(now_us - last_us_) / 1e6;
        if (dt > 0.0) {
            tokens_ = (std::min)(burst_, tokens_ + dt * rate_);
            last_us_ = now_us;
        }
    }
};

// Exponentially weighted send-rate estimate in bytes per second. Not thread-safe.
class RateEstimator {
public:
    explicit RateEstimator(double window_seconds = 1.0) : window_s_(window_seconds) {}

    void add(int64_t now_us, size_t bytes) {
        if (last_us_ == 0) { last_us_ = now_us; acc_ = 0; }
        acc_ += static_cast<double>(bytes);
        const double dt = static_cast<double>(now_us - last_us_) / 1e6;
        if (dt >= window_s_) {
            const double inst = acc_ / dt;
            rate_ = have_ ? 0.5 * rate_ + 0.5 * inst : inst;
            have_ = true;
            acc_ = 0;
            last_us_ = now_us;
        }
    }

    // Falls back to `fallback` until a full window has been observed.
    double bytes_per_second(double fallback) const { return have_ ? rate_ : fallback; }
    bool   ready() const { return have_; }

private:
    double  window_s_;
    double  rate_ = 0.0;
    double  acc_ = 0.0;
    int64_t last_us_ = 0;
    bool    have_ = false;
};

// Coalescer: allows one event per `interval_us` per key. Not thread-safe.
class Coalescer {
public:
    explicit Coalescer(int64_t interval_us) : interval_us_(interval_us) {}
    // true if an event is allowed now (and records it).
    bool allow(int64_t now_us) {
        if (last_us_ != 0 && now_us - last_us_ < interval_us_) return false;
        last_us_ = now_us;
        return true;
    }
private:
    int64_t interval_us_;
    int64_t last_us_ = 0;
};

} // namespace parties
