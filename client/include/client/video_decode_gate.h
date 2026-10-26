#pragma once

#include <cstdint>

namespace parties::client {

enum class VideoDecodeDecision {
    Accept,
    DropUntilKeyframe,
    Discontinuity,
};

class VideoDecodeGate {
public:
    VideoDecodeDecision on_frame(uint32_t frame_number, bool keyframe) {
        if (awaiting_keyframe_) {
            if (!keyframe) return VideoDecodeDecision::DropUntilKeyframe;
            awaiting_keyframe_ = false;
            have_last_frame_ = true;
            last_frame_ = frame_number;
            return VideoDecodeDecision::Accept;
        }

        if (keyframe) {
            have_last_frame_ = true;
            last_frame_ = frame_number;
            return VideoDecodeDecision::Accept;
        }

        if (!have_last_frame_ || frame_number != last_frame_ + 1u) {
            require_keyframe();
            return VideoDecodeDecision::Discontinuity;
        }

        last_frame_ = frame_number;
        return VideoDecodeDecision::Accept;
    }

    void require_keyframe() {
        awaiting_keyframe_ = true;
        have_last_frame_ = false;
    }

    bool awaiting_keyframe() const { return awaiting_keyframe_; }

private:
    uint32_t last_frame_ = 0;
    bool have_last_frame_ = false;
    bool awaiting_keyframe_ = true;
};

}
