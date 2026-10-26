#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace parties {

constexpr int64_t VIDEO_BACKLOG_MIN_THRESHOLD_BYTES = 64 * 1024;
constexpr int64_t VIDEO_BACKLOG_MAX_THRESHOLD_BYTES = 2 * 1024 * 1024;
constexpr double  VIDEO_BACKLOG_QUEUE_SECONDS       = 0.25;
constexpr uint32_t VIDEO_BACKLOG_DEFAULT_RTT_US     = 50'000;
constexpr uint32_t VIDEO_BACKLOG_MAX_RTT_US         = 1'000'000;

class MinRtt {
public:
    static constexpr MinRtt from_quic_min_rtt_us(uint32_t us) { return MinRtt{us}; }
    static constexpr MinRtt assumed_default() { return MinRtt{VIDEO_BACKLOG_DEFAULT_RTT_US}; }
    constexpr uint32_t us() const { return us_; }

private:
    constexpr explicit MinRtt(uint32_t us) : us_(us) {}
    uint32_t us_;
};

struct ViewerBacklogInputs {
    double  viewer_rate_bytes_per_s = 0.0;
    MinRtt  min_rtt = MinRtt::assumed_default();
    int64_t keyframe_headroom_bytes = 0;
};

constexpr int64_t viewer_backlog_threshold_bytes(const ViewerBacklogInputs& in) {
    const uint32_t rtt = (std::min)(in.min_rtt.us(), VIDEO_BACKLOG_MAX_RTT_US);
    const double window_s = VIDEO_BACKLOG_QUEUE_SECONDS + static_cast<double>(rtt) / 1e6;
    const double raw = in.viewer_rate_bytes_per_s * window_s;
    int64_t v = raw <= 0.0 ? 0 : static_cast<int64_t>(raw);
    if (in.keyframe_headroom_bytes > 0) v += in.keyframe_headroom_bytes;
    return std::clamp<int64_t>(v, VIDEO_BACKLOG_MIN_THRESHOLD_BYTES,
                               VIDEO_BACKLOG_MAX_THRESHOLD_BYTES);
}

enum class GateDecision { Forward, Drop, DropAndRequestKeyframe };

class ViewerVideoGate {
public:
    [[nodiscard]] GateDecision admit(int64_t outstanding_bytes, int64_t threshold_bytes, bool keyframe) {
        if (outstanding_bytes > threshold_bytes) {
            awaiting_keyframe_ = true;
            return GateDecision::Drop;
        }
        if (awaiting_keyframe_) {
            if (!keyframe) return GateDecision::DropAndRequestKeyframe;
            awaiting_keyframe_ = false;
        }
        return GateDecision::Forward;
    }

    void require_keyframe() { awaiting_keyframe_ = true; }
    bool awaiting_keyframe() const { return awaiting_keyframe_; }

private:
    bool awaiting_keyframe_ = false;
};

class TokenBucket {
public:
    TokenBucket(double tokens_per_second, double burst)
        : rate_(tokens_per_second), burst_(burst), tokens_(burst) {}

    [[nodiscard]] bool try_take(int64_t now_us, double cost = 1.0) {
        refill(now_us);
        if (tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

private:
    double rate_;
    double burst_;
    double tokens_;
    std::optional<int64_t> last_us_;

    void refill(int64_t now_us) {
        if (!last_us_) { last_us_ = now_us; return; }
        const double dt = static_cast<double>(now_us - *last_us_) / 1e6;
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
        if (!window_start_us_) { window_start_us_ = now_us; acc_ = 0; }
        acc_ += static_cast<double>(bytes);
        const double dt = static_cast<double>(now_us - *window_start_us_) / 1e6;
        if (dt >= window_s_) {
            const double inst = acc_ / dt;
            rate_ = rate_ ? 0.5 * *rate_ + 0.5 * inst : inst;
            acc_ = 0;
            window_start_us_ = now_us;
        }
    }

    std::optional<double> bytes_per_second() const { return rate_; }

private:
    double window_s_;
    std::optional<double> rate_;
    double acc_ = 0.0;
    std::optional<int64_t> window_start_us_;
};

class Coalescer {
public:
    explicit Coalescer(int64_t interval_us) : interval_us_(interval_us) {}

    [[nodiscard]] bool allow(int64_t now_us) {
        if (last_us_ && now_us - *last_us_ < interval_us_) return false;
        last_us_ = now_us;
        return true;
    }

private:
    int64_t interval_us_;
    std::optional<int64_t> last_us_;
};

}
