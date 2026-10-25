#pragma once

#include <parties/video_common.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace parties {

struct VideoFrameReorderConfig {
    uint32_t max_hold_ms          = 150;
    size_t   max_held_frames      = 8;
    size_t   max_bytes            = 8u * 1024u * 1024u;
};

class VideoFrameReorderBuffer {
public:
    using DeliverFn = std::function<void(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame)>;
    using LostFn    = std::function<void(uint32_t first_missing_seq)>;

    VideoFrameReorderBuffer();
    explicit VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg);

    void on_frame(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame,
                  int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    void on_frame_aborted(uint32_t frame_seq, int64_t now_us,
                          const DeliverFn& deliver, const LostFn& lost);

    void poll(int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    void reset();

    size_t held_frames() const { return held_.size(); }
    size_t held_bytes() const { return held_bytes_; }
    bool   has_baseline() const { return have_last_; }
    bool   needs_keyframe() const { return !have_last_ || awaiting_keyframe_; }
    uint32_t last_delivered() const { return last_delivered_; }

private:
    struct Held {
        VideoFrameHeader hdr;
        std::vector<uint8_t> frame;
        int64_t arrived_us = 0;
    };

    VideoFrameReorderConfig cfg_;
    bool     have_last_ = false;
    bool     awaiting_keyframe_ = true;
    uint32_t last_delivered_ = 0;
    std::map<int64_t, Held> held_;
    size_t   held_bytes_ = 0;
    int64_t  gap_since_us_ = 0;

    void deliver_one(Held&& h, const DeliverFn& deliver);
    void drain(const DeliverFn& deliver);
    void require_keyframe(const LostFn& lost);
    void bound_held();
    int64_t key_for(uint32_t seq) const;
};

}
