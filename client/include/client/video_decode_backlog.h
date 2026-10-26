#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>

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
VideoDecodeBacklogTrim trim_to_latest_keyframe(std::deque<Work>& frames, IsKeyframe&& is_keyframe) {
    const auto newest_keyframe = std::find_if(frames.rbegin(), frames.rend(), is_keyframe);
    if (newest_keyframe == frames.rend()) return {};
    const auto keep_from = std::prev(newest_keyframe.base());
    const auto dropped = static_cast<size_t>(keep_from - frames.begin());
    frames.erase(frames.begin(), keep_from);
    return {true, dropped};
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
