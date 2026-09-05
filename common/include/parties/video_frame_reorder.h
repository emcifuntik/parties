#pragma once

// Reorder buffer for frames arriving on per-frame QUIC streams.
//
// Per-frame streams complete independently, so frame N+1 can finish before a
// frame N that lost a packet. The decode pipeline (VideoDecodeGate on Windows,
// awaiting_keyframe_ on Apple) requires frames strictly in frame_seq order, so
// this buffer:
//   * retains bounded startup deltas until the first keyframe establishes order;
//   * delivers a frame immediately when it is the next expected one, then
//     drains any consecutive held frames;
//   * holds newer complete frames while an older one is missing, bounded by
//     max_hold_ms / max_held_frames / max_bytes;
//   * on timeout, abort of the missing frame, or overflow, reports the gap once
//     via `lost` and resumes from the oldest held frame (the decode gate then
//     sees a discontinuity and the caller requests a keyframe);
//   * delivers a held keyframe immediately: nothing before a keyframe matters;
//   * ignores frames older than the last delivered one and duplicates;
//   * resets its baseline only when the owner explicitly resets the share.
// All frame_seq comparisons are wrap-safe (video_seq_newer / video_seq_distance).
//
// Not thread-safe: the owner serializes on_frame / on_frame_aborted / poll.
//
// Implementation: common/src/video_frame_reorder.cpp
// Tests:          tests/video_frame_reorder_test.cpp

#include <parties/video_common.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace parties {

struct VideoFrameReorderConfig {
    uint32_t max_hold_ms          = 150;                 // wait this long for a missing frame
    size_t   max_held_frames      = 8;                   // then give up on the gap
    size_t   max_bytes            = 8u * 1024u * 1024u;  // total bytes held
};

class VideoFrameReorderBuffer {
public:
    // `frame` is the full [14-byte header][encoded] buffer the decode path expects.
    using DeliverFn = std::function<void(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame)>;
    // Called once per gap that is resumed at a DELTA frame (timeout, abort,
    // overflow) with the first missing frame_seq — the decoder will need a
    // keyframe. A gap closed by a keyframe is NOT reported: the keyframe
    // re-anchors the decoder by itself (the server deliberately resumes a
    // throttled viewer at a keyframe, so this is the normal recovery path).
    using LostFn    = std::function<void(uint32_t first_missing_seq)>;

    VideoFrameReorderBuffer();
    explicit VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg);

    // A complete frame arrived. `hdr` must be the parsed header of `frame`.
    void on_frame(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame,
                  int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    // The stream carrying `frame_seq` was aborted before completing: the frame
    // will never arrive. Unblocks anything held behind it.
    void on_frame_aborted(uint32_t frame_seq, int64_t now_us,
                          const DeliverFn& deliver, const LostFn& lost);

    // Timer-driven give-up for gaps at the tail (no newer frames arriving).
    // Call periodically (every UI tick is fine).
    void poll(int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    // Forget everything (watch stopped / disconnected). Wait for a keyframe.
    void reset();

    size_t held_frames() const { return held_.size(); }
    size_t held_bytes() const { return held_bytes_; }
    bool   has_baseline() const { return have_last_; }
    uint32_t last_delivered() const { return last_delivered_; }

private:
    struct Held {
        VideoFrameHeader hdr;
        std::vector<uint8_t> frame;
        int64_t arrived_us = 0;
    };

    VideoFrameReorderConfig cfg_;
    bool     have_last_ = false;
    uint32_t last_delivered_ = 0;
    // Keyed by frame_seq re-based so that std::map ordering matches wrap-safe
    // ordering relative to last_delivered_ (see .cpp).
    std::map<int64_t, Held> held_;
    size_t   held_bytes_ = 0;
    int64_t  gap_since_us_ = 0;   // when we first started waiting for the current gap

    void deliver_one(Held&& h, const DeliverFn& deliver);
    void drain(const DeliverFn& deliver);
    void skip_gap(int64_t now_us, const DeliverFn& deliver, const LostFn& lost);
    int64_t key_for(uint32_t seq) const;
};

} // namespace parties
