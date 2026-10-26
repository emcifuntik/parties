#pragma once

#include <parties/video_common.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace parties {

struct VideoFrameReorderConfig {
    uint32_t max_hold_ms          = 150;
    size_t   max_held_frames      = 240;
    size_t   max_bytes            = 8u * 1024u * 1024u;
};

enum class KeyframeRequest : bool { None = false, Needed = true };

struct ReorderOutcome {
    std::vector<ParsedVideoFrame> deliver;
    KeyframeRequest               keyframe = KeyframeRequest::None;
};

class VideoFrameReorderBuffer {
public:
    VideoFrameReorderBuffer();
    explicit VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg);

    VideoFrameReorderBuffer(VideoFrameReorderBuffer&&) noexcept = default;
    VideoFrameReorderBuffer& operator=(VideoFrameReorderBuffer&&) noexcept = default;
    VideoFrameReorderBuffer(const VideoFrameReorderBuffer&) = delete;
    VideoFrameReorderBuffer& operator=(const VideoFrameReorderBuffer&) = delete;

    [[nodiscard]] ReorderOutcome on_frame(ParsedVideoFrame&& frame, int64_t now_us);
    [[nodiscard]] KeyframeRequest on_frame_aborted(uint32_t frame_seq);
    [[nodiscard]] KeyframeRequest poll(int64_t now_us);

    void reset();

    bool wants_pli_retry() const { return !have_last_ || awaiting_keyframe_; }

    size_t held_frames() const { return held_.size(); }
    size_t held_bytes() const { return held_bytes_; }
    bool   has_baseline() const { return have_last_; }
    uint32_t last_delivered() const { return last_delivered_; }

private:
    struct Held {
        ParsedVideoFrame frame;
        int64_t arrived_us = 0;
    };

    VideoFrameReorderConfig cfg_;
    bool     have_last_ = false;
    bool     awaiting_keyframe_ = true;
    uint32_t last_delivered_ = 0;
    std::map<int64_t, Held> held_;
    size_t   held_bytes_ = 0;
    std::optional<int64_t> gap_since_us_;

    void drain(ReorderOutcome& out);
    KeyframeRequest require_keyframe();
    void bound_held();
    int64_t key_for(uint32_t seq) const;
};

}
