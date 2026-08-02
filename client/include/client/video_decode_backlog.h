#pragma once

#include <cstddef>
#include <queue>
#include <utility>

namespace parties::client {

inline constexpr size_t kVideoDecodeBacklogWarningFrames = 10;
inline constexpr size_t kVideoDecodeBacklogHardLimitFrames = 240;

struct VideoDecodeBacklogTrim {
    bool found_keyframe = false;
    size_t dropped = 0;
};

// Retains the newest independently decodable reference chain. If no keyframe is
// present, the queue is left intact because every delta frame is still needed.
template <typename Work, typename IsKeyframe>
VideoDecodeBacklogTrim trim_to_latest_keyframe(
        std::queue<Work>& frames, IsKeyframe&& is_keyframe) {
    std::queue<Work> retained;
    VideoDecodeBacklogTrim result;
    while (!frames.empty()) {
        Work work = std::move(frames.front());
        frames.pop();
        if (is_keyframe(work)) {
            result.dropped += retained.size();
            while (!retained.empty()) retained.pop();
            result.found_keyframe = true;
        }
        retained.push(std::move(work));
    }
    frames.swap(retained);
    return result;
}

inline bool should_resync_decode_backlog(size_t retained_frames,
                                         bool found_keyframe,
                                         bool decoder_ready) {
    return decoder_ready && !found_keyframe &&
           retained_frames > kVideoDecodeBacklogHardLimitFrames;
}

} // namespace parties::client
