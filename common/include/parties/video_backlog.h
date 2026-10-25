#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace parties {

constexpr int64_t VIDEO_BACKLOG_MIN_THRESHOLD_BYTES = 64 * 1024;
constexpr int64_t VIDEO_BACKLOG_MAX_THRESHOLD_BYTES = 2 * 1024 * 1024;
constexpr double  VIDEO_BACKLOG_QUEUE_SECONDS       = 0.25;
constexpr uint32_t VIDEO_BACKLOG_DEFAULT_RTT_US     = 50'000;
constexpr uint32_t VIDEO_BACKLOG_MAX_RTT_US         = 1'000'000;

constexpr int64_t video_backlog_threshold_bytes(double send_rate_bytes_per_s) {
    const double raw = send_rate_bytes_per_s * VIDEO_BACKLOG_QUEUE_SECONDS;
    const int64_t v = raw <= 0.0 ? 0 : static_cast<int64_t>(raw);
    return std::clamp<int64_t>(v, VIDEO_BACKLOG_MIN_THRESHOLD_BYTES,
                               VIDEO_BACKLOG_MAX_THRESHOLD_BYTES);
}

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

struct ViewerVideoGate {
    bool awaiting_keyframe = false;

    bool admit(int64_t outstanding, int64_t threshold, bool keyframe, bool* needs_keyframe) {
        if (needs_keyframe) *needs_keyframe = false;
        if (outstanding > threshold) {
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

    double bytes_per_second(double fallback) const { return have_ ? rate_ : fallback; }
    bool   ready() const { return have_; }

private:
    double  window_s_;
    double  rate_ = 0.0;
    double  acc_ = 0.0;
    int64_t last_us_ = 0;
    bool    have_ = false;
};

class Coalescer {
public:
    explicit Coalescer(int64_t interval_us) : interval_us_(interval_us) {}
    bool allow(int64_t now_us) {
        if (last_us_ != 0 && now_us - last_us_ < interval_us_) return false;
        last_us_ = now_us;
        return true;
    }
private:
    int64_t interval_us_;
    int64_t last_us_ = 0;
};

}
