#pragma once

#include <parties/video_common.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace parties {

struct VideoSendControllerConfig {
    uint32_t target_bitrate_bps   = VIDEO_DEFAULT_BITRATE;
    uint32_t min_bitrate_bps      = VIDEO_ADAPT_MIN_BITRATE;
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
    double   min_reconfigure_delta = 0.10;
};

class VideoSendController {
public:
    VideoSendController();
    explicit VideoSendController(const VideoSendControllerConfig& cfg);

    void reset(const VideoSendControllerConfig& cfg);
    void reset();
    void clear_outstanding();

    void set_target_bitrate(uint32_t bps);

    void on_enqueued(size_t bytes);
    void on_completed(size_t bytes);
    void on_send_failed(size_t bytes);
    void on_encoder_drop();
    int64_t outstanding_bytes() const;

    bool admit_frame(int64_t now_us);

    void on_connection_stats(int64_t now_us, uint32_t rtt_us,
                             uint64_t suspected_lost_packets,
                             uint64_t spurious_lost_packets,
                             uint64_t sent_packets);

    void tick(int64_t now_us);

    uint32_t bitrate_bps() const;

    std::optional<uint32_t> take_bitrate_update();
    bool take_keyframe_request();

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
public:
    static uint32_t queue_delay_ms(int64_t outstanding_bytes, uint32_t bitrate_bps, uint32_t rtt_us);

private:
    VideoSendControllerConfig cfg_;

    std::atomic<int64_t>  outstanding_{0};
    std::atomic<int64_t>  allowance_bytes_{0};
    std::atomic<uint32_t> bitrate_bps_{0};
    std::atomic<uint32_t> pending_bitrate_{0};
    std::atomic<uint32_t> admission_drops_{0};
    std::atomic<uint32_t> admission_drops_window_{0};
    std::atomic<uint32_t> send_failures_{0};
    std::atomic<uint32_t> encoder_drops_window_{0};
    std::atomic<bool>     keyframe_request_{false};

    uint32_t rtt_us_ = 0;
    int64_t  window_start_us_ = 0;
    uint32_t congested_windows_ = 0;
    uint32_t clean_windows_ = 0;
    uint32_t last_queue_delay_ms_ = 0;
    uint32_t peak_queue_delay_ms_ = 0;
    double   last_loss_ratio_ = 0.0;
    bool     have_stats_ = false;
    uint64_t last_suspected_lost_ = 0;
    uint64_t last_spurious_lost_ = 0;
    uint64_t last_sent_packets_ = 0;
    uint64_t window_lost_ = 0;
    uint64_t window_sent_ = 0;
    uint32_t published_bitrate_ = 0;
    int64_t  last_publish_us_ = 0;
    uint32_t decreases_ = 0;

    void publish_bitrate_if_needed(int64_t now_us, bool force);
    void refresh_allowance();
};

}
