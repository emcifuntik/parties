#pragma once

#include <parties/video_common.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace parties {

struct VideoSendControllerConfig {
    uint32_t ceiling_bitrate_bps  = VIDEO_DEFAULT_BITRATE;
    uint32_t floor_bitrate_bps    = VIDEO_ADAPT_MIN_BITRATE;
    uint32_t queue_budget_ms      = 120;
    uint32_t congestion_queue_ms  = 80;
    uint32_t window_ms            = 500;
    uint32_t congested_windows    = 2;
    uint32_t clean_windows        = 4;
    double   decrease_factor      = 0.8;
    double   increase_factor      = 1.08;
    double   loss_threshold       = 0.03;
    uint32_t default_rtt_us       = 50'000;
    uint32_t min_reconfigure_ms   = 500;
    double   min_reconfigure_ratio = 0.10;
};

struct QuicPathCounters {
    uint32_t rtt_us = 0;
    uint64_t cumulative_suspected_lost_packets = 0;
    uint64_t cumulative_spurious_lost_packets  = 0;
    uint64_t cumulative_sent_packets           = 0;
};

namespace video_send_detail {

uint32_t queue_delay_ms(int64_t outstanding_bytes, uint32_t bitrate_bps, uint32_t rtt_us);

struct SharedCounters {
    std::atomic<int64_t>  outstanding{0};
    std::atomic<int64_t>  allowance_bytes{0};
    std::atomic<uint32_t> bitrate_bps{0};
    std::atomic<uint32_t> pending_bitrate{0};
    std::atomic<uint32_t> admission_drops{0};
    std::atomic<uint32_t> admission_drops_window{0};
    std::atomic<uint32_t> send_failures{0};
    std::atomic<uint32_t> send_failures_window{0};
    std::atomic<bool>     keyframe_request{false};
};

}

class VideoSendAccounting {
public:
    void on_bytes_queued(size_t bytes);
    void on_bytes_retired(size_t bytes);
    void on_send_failed();
    [[nodiscard]] bool admit_frame();
    uint32_t bitrate_bps() const;

private:
    friend class VideoSendController;
    explicit VideoSendAccounting(video_send_detail::SharedCounters& c) : c_(c) {}
    video_send_detail::SharedCounters& c_;
};

class VideoEncodeFeedback {
public:
    [[nodiscard]] std::optional<uint32_t> take_bitrate_update();
    [[nodiscard]] bool take_keyframe_request();

private:
    friend class VideoSendController;
    explicit VideoEncodeFeedback(video_send_detail::SharedCounters& c) : c_(c) {}
    video_send_detail::SharedCounters& c_;
};

class VideoSendController {
public:
    explicit VideoSendController(const VideoSendControllerConfig& cfg = {});
    VideoSendController(const VideoSendController&) = delete;
    VideoSendController& operator=(const VideoSendController&) = delete;

    void start_share(const VideoSendControllerConfig& cfg);
    void on_connection_lost();
    void set_ceiling_bitrate(uint32_t bps);
    void on_path_counters(const QuicPathCounters& counters);
    void tick(int64_t now_us);
    int64_t outstanding_bytes() const;

    VideoSendAccounting& accounting() { return accounting_; }
    VideoEncodeFeedback& encode_feedback() { return feedback_; }

    struct Stats {
        uint32_t bitrate_bps        = 0;
        int64_t  outstanding_bytes  = 0;
        uint32_t rtt_us             = 0;
        uint32_t queue_delay_ms     = 0;
        double   loss_ratio         = 0.0;
        uint32_t admission_drops    = 0;
        uint32_t send_failures      = 0;
        uint32_t decreases          = 0;
    };
    Stats stats() const;

private:
    VideoSendControllerConfig cfg_;
    video_send_detail::SharedCounters counters_;
    VideoSendAccounting accounting_{counters_};
    VideoEncodeFeedback feedback_{counters_};

    uint32_t rtt_us_ = 0;
    std::optional<int64_t> window_start_us_;
    uint32_t congested_windows_ = 0;
    uint32_t clean_windows_ = 0;
    uint32_t last_queue_delay_ms_ = 0;
    uint32_t peak_queue_delay_ms_ = 0;
    double   last_loss_ratio_ = 0.0;
    std::optional<QuicPathCounters> last_counters_;
    uint64_t window_lost_ = 0;
    uint64_t window_sent_ = 0;
    uint32_t published_bitrate_ = 0;
    std::optional<int64_t> last_publish_us_;
    uint32_t decreases_ = 0;

    uint32_t floor_bps() const;
    void publish_bitrate(std::optional<int64_t> at_us);
    void publish_bitrate_if_due(int64_t now_us);
    void refresh_allowance();
};

}
