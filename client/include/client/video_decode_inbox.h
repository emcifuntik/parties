#pragma once

#include <client/video_decode_backlog.h>
#include <client/video_decode_gate.h>
#include <parties/video_common.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace parties::client {

static_assert(kVideoDecodeStaleFlushRecoveryUs <=
                  static_cast<int64_t>(VIDEO_KEYFRAME_INTERVAL_MS) * 1000,
              "kVideoDecodeStaleFlushRecoveryUs must fit in VIDEO_KEYFRAME_INTERVAL_MS");

enum class StreamOrigin { RemoteSharer, SelfPreview };

enum class KeyframeRecovery {
    None,
    RequestFromSharer,
    AwaitLocalEncoder,
    AwaitPeriodicKeyframe,
    Suppressed,
};

struct DecodeWork {
    ParsedVideoFrame frame;
    int64_t arrival_us = 0;
    bool recovery_keyframe = false;

    bool keyframe() const { return frame.header().keyframe; }
};

class VideoDecodeInbox {
public:
    explicit VideoDecodeInbox(StreamOrigin origin) : origin_(origin) {}

    [[nodiscard]] KeyframeRecovery push(ParsedVideoFrame&& frame, int64_t now_us) {
        std::lock_guard lock(m_);
        const VideoFrameHeader& header = frame.header();
        const bool recovery_keyframe = header.keyframe && gate_.awaiting_keyframe();
        switch (gate_.on_frame(header.frame_seq, header.keyframe)) {
        case VideoDecodeDecision::Discontinuity:
            queue_.clear();
            return request_unless_self_preview();
        case VideoDecodeDecision::DropUntilKeyframe:
            return KeyframeRecovery::None;
        case VideoDecodeDecision::Accept:
            queue_.push_back({std::move(frame), now_us, recovery_keyframe});
            cv_.notify_one();
            return KeyframeRecovery::None;
        }
        return KeyframeRecovery::None;
    }

    [[nodiscard]] std::optional<std::deque<DecodeWork>> wait_batch(std::stop_token stop) {
        std::unique_lock lock(m_);
        cv_.wait(lock, stop, [this] { return !queue_.empty(); });
        if (stop.stop_requested()) return std::nullopt;
        return std::exchange(queue_, {});
    }

    [[nodiscard]] KeyframeRecovery resync_after_stale_backlog() {
        std::lock_guard lock(m_);
        discard_until_keyframe();
        const bool request = stale_.on_stale_flush();
        if (stale_.stale_flushes_in_a_row == kVideoDecodeStaleFlushPliLimit + 1)
            return KeyframeRecovery::AwaitPeriodicKeyframe;
        if (!request) return KeyframeRecovery::Suppressed;
        return request_unless_self_preview();
    }

    [[nodiscard]] KeyframeRecovery reset_after_decoder_failure() {
        std::lock_guard lock(m_);
        discard_until_keyframe();
        return request_unless_self_preview();
    }

    void on_keyframe_decoded(int64_t now_us, bool recovery_keyframe) {
        std::lock_guard lock(m_);
        stale_.on_keyframe_decoded(now_us, recovery_keyframe);
    }

    bool stale_flushes_suppressed() const {
        std::lock_guard lock(m_);
        return stale_.suppressed();
    }

    bool wants_pli_retry() const {
        std::lock_guard lock(m_);
        return origin_ == StreamOrigin::RemoteSharer && !stale_.suppressed() &&
               gate_.awaiting_keyframe();
    }

    StreamOrigin origin() const { return origin_; }

private:
    void discard_until_keyframe() {
        gate_.require_keyframe();
        queue_.clear();
    }

    KeyframeRecovery request_unless_self_preview() const {
        return origin_ == StreamOrigin::SelfPreview ? KeyframeRecovery::AwaitLocalEncoder
                                                    : KeyframeRecovery::RequestFromSharer;
    }

    mutable std::mutex m_;
    std::condition_variable_any cv_;
    std::deque<DecodeWork> queue_;
    VideoDecodeGate gate_;
    VideoDecodeStaleFlushTracker stale_;
    const StreamOrigin origin_;
};

}
