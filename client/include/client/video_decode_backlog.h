#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <utility>

namespace parties::client {

inline constexpr size_t kVideoDecodeBacklogWarningFrames = 10;
// A contiguous delta chain longer than this (one full second at 60 fps) is a
// sustained overload, not a transient hiccup: flush and resync at a keyframe.
inline constexpr size_t kVideoDecodeBacklogHardLimitFrames = 60;
// Age rule: once the oldest queued frame has waited this long the picture is
// already visibly behind, whatever the queue length.
inline constexpr int64_t kVideoDecodeBacklogMaxAgeUs = 500'000;

// Anti-flap for the flush rules. A decoder that is persistently slower than
// the stream (dav1d 4K AV1 on a weak CPU, a busy GPU, the self-preview
// competing with the encoder) would otherwise turn every flush into a PLI:
// flush -> PLI -> keyframe (the most expensive frame) -> deltas back up ->
// flush again, at ~1 Hz, spiking the sharer's bitrate and dropping frames for
// every viewer that was keeping up. Only the first kVideoDecodeStaleFlushPliLimit
// consecutive flushes may request a keyframe; after that the stream waits for
// the sharer's periodic keyframe.
inline constexpr uint32_t kVideoDecodeStaleFlushPliLimit = 2;
// A run of flushes ends only once the decoder has demonstrably kept up: a
// keyframe decoded in sequence (not the recovery keyframe the gate was waiting
// for — every flush is necessarily followed by one of those) at least this
// long after decoding resumed. Sized to most of one sharer keyframe interval
// (VIDEO_KEYFRAME_INTERVAL_MS = 5 s, checked by a static_assert in app.cpp) so
// a periodic keyframe landing shortly after a recovery cannot end the run,
// while one interval of clean decoding always does despite delivery jitter.
inline constexpr int64_t kVideoDecodeStaleFlushRecoveryUs = 4'000'000;

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

// Age variant of the same policy: a warm decoder whose oldest queued frame is
// older than kVideoDecodeBacklogMaxAgeUs, with no keyframe queued to jump to,
// should drop the backlog and request a fresh random-access point. A cold
// decoder (still initializing) is exempt — its backlog is the normal warm-up.
inline bool should_resync_stale_decode_backlog(int64_t oldest_arrival_us,
                                               int64_t now_us,
                                               bool found_keyframe,
                                               bool decoder_ready) {
    return decoder_ready && !found_keyframe &&
           (now_us - oldest_arrival_us) >= kVideoDecodeBacklogMaxAgeUs;
}

// True when the given consecutive stale flush may still request a keyframe.
inline bool should_request_keyframe_after_stale_flush(uint32_t consecutive_flushes) {
    return consecutive_flushes <= kVideoDecodeStaleFlushPliLimit;
}

// Per-stream state of the anti-flap rule. Pure; the owner serializes access.
struct VideoDecodeStaleFlushTracker {
    uint32_t stale_flushes_in_a_row = 0;
    // Decoding resumed after the last flush (its recovery keyframe was
    // decoded) at resumed_us; nothing has resumed yet while `resumed` is false.
    bool    resumed = false;
    int64_t resumed_us = 0;

    // Records a whole-batch flush. True when this flush may request a keyframe.
    bool on_stale_flush() {
        ++stale_flushes_in_a_row;
        resumed = false;
        return should_request_keyframe_after_stale_flush(stale_flushes_in_a_row);
    }

    // A keyframe was decoded. `recovery` means the gate was waiting for a
    // keyframe when it was accepted (the frame that ends a flush, a
    // discontinuity or the stream start): decoding resumes, but that proves
    // nothing about the decoder's speed. An in-sequence keyframe decoded at
    // least kVideoDecodeStaleFlushRecoveryUs after resumption ends the run.
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

    // True while keyframe requests are suppressed: the stream waits for the
    // sharer's periodic keyframe instead of asking for one.
    bool suppressed() const {
        return !should_request_keyframe_after_stale_flush(stale_flushes_in_a_row);
    }
};

} // namespace parties::client
