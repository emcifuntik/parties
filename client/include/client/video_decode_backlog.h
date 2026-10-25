#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <utility>

namespace parties::client {

inline constexpr size_t kVideoDecodeBacklogWarningFrames = 10;
inline constexpr size_t kVideoDecodeBacklogHardLimitFrames = 60;
inline constexpr int64_t kVideoDecodeBacklogMaxAgeUs = 500'000;

inline constexpr uint32_t kVideoDecodeStaleFlushPliLimit = 2;
inline constexpr int64_t kVideoDecodeStaleFlushRecoveryUs = 4'000'000;

struct VideoDecodeBacklogTrim {
    bool found_keyframe = false;
    size_t dropped = 0;
};

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

inline bool should_resync_stale_decode_backlog(int64_t oldest_arrival_us,
                                               int64_t now_us,
                                               bool found_keyframe,
                                               bool decoder_ready) {
    return decoder_ready && !found_keyframe &&
           (now_us - oldest_arrival_us) >= kVideoDecodeBacklogMaxAgeUs;
}

inline bool should_request_keyframe_after_stale_flush(uint32_t consecutive_flushes) {
    return consecutive_flushes <= kVideoDecodeStaleFlushPliLimit;
}

struct VideoDecodeStaleFlushTracker {
    uint32_t stale_flushes_in_a_row = 0;
    bool    resumed = false;
    int64_t resumed_us = 0;

    bool on_stale_flush() {
        ++stale_flushes_in_a_row;
        resumed = false;
        return should_request_keyframe_after_stale_flush(stale_flushes_in_a_row);
    }

    void on_keyframe_decoded(int64_t now_us, bool recovery) {
        if (stale_flushes_in_a_row == 0) return;
        if (recovery || !resumed) {
            resumed = true;
            resumed_us = now_us;
            return;
        }
        if (now_us - resumed_us >= kVideoDecodeStaleFlushRecoveryUs) {
            stale_flushes_in_a_row = 0;
            resumed = false;
        }
    }

    bool suppressed() const {
        return !should_request_keyframe_after_stale_flush(stale_flushes_in_a_row);
    }
};

}
