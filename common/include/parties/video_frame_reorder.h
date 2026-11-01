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
//   * holds newer complete frames while an older one is missing. The streams
//     are reliable, so a missing frame is almost always a retransmission still
//     in flight: the hold scales with the path RTT (set_rtt), from
//     min_hold_ms up to max_hold_ms;
//   * on timeout, abort of the missing frame, or overflow, reports the gap once
//     via `lost` so the owner requests a keyframe, but keeps holding: whichever
//     of the late frame or a keyframe arrives first resumes delivery without
//     advancing over broken deltas;
//   * does not report a gap while a newer keyframe is already arriving
//     (on_frame_started saw its header): that keyframe repairs the gap, and a
//     keyframe request would only force another one;
//   * remembers aborted frames ahead of the chain, so reaching one fails fast;
//   * delivers a held keyframe immediately: nothing before a keyframe matters;
//   * ignores frames older than the last delivered one and duplicates; a
//     restarted share (its counter starts over) needs an explicit reset();
//   * when over its bounds, drops the newest held frames: the oldest ones are
//     what a late retransmission needs to resume.
// All frame_seq comparisons are wrap-safe (video_seq_newer / video_seq_distance).
//
// Not thread-safe: the owner serializes every call.
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
    // Hold for a missing frame: rtt_hold_factor x RTT + hold_extra_ms, clamped
    // to [min_hold_ms, max_hold_ms]; min_hold_ms until the RTT is known.
    uint32_t min_hold_ms          = 150;
    uint32_t max_hold_ms          = 1500;
    uint32_t rtt_hold_factor      = 3;
    uint32_t hold_extra_ms        = 100;
    size_t   max_held_frames      = 240;                 // drop the newest beyond this
    size_t   max_bytes            = 8u * 1024u * 1024u;  // total bytes held
};

class VideoFrameReorderBuffer {
public:
    // `frame` is the full [14-byte header][encoded] buffer the decode path expects.
    using DeliverFn = std::function<void(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame)>;
    // Called once per broken reference chain (timeout, abort, overflow) with
    // the first missing frame_seq, unless a newer keyframe is already arriving.
    // No more deltas are delivered until a keyframe arrives. A gap closed by a
    // keyframe is NOT reported: the keyframe re-anchors the decoder by itself
    // (the server deliberately resumes a throttled viewer at a keyframe, so
    // this is the normal recovery path).
    using LostFn    = std::function<void(uint32_t first_missing_seq)>;

    VideoFrameReorderBuffer();
    explicit VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg);

    // Smoothed RTT of the path the frames arrive on (0 = unknown).
    void set_rtt(uint32_t rtt_us) { rtt_us_ = rtt_us; }
    // The hold for a missing frame at the current RTT.
    int64_t hold_us() const;

    // The first bytes of a frame's stream arrived (its header): the frame is
    // on its way. Only keyframes matter here (see the class comment).
    void on_frame_started(const VideoFrameHeader& hdr, int64_t now_us);

    // A complete frame arrived. `hdr` must be the parsed header of `frame`.
    void on_frame(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame,
                  int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    // The stream carrying `frame_seq` was aborted before completing: the frame
    // will never arrive. Enters keyframe recovery when it breaks the chain.
    void on_frame_aborted(uint32_t frame_seq, int64_t now_us,
                          const DeliverFn& deliver, const LostFn& lost);

    // Timer-driven give-up for gaps at the tail (no newer frames arriving).
    // Call periodically (every UI tick is fine).
    void poll(int64_t now_us, const DeliverFn& deliver, const LostFn& lost);

    // Forget everything (watch stopped / share restarted). Wait for a keyframe.
    void reset();

    size_t held_frames() const { return held_.size(); }
    size_t held_bytes() const { return held_bytes_; }
    bool   has_baseline() const { return have_last_; }
    // No usable reference: waiting for a keyframe.
    bool   needs_keyframe() const { return !have_last_ || awaiting_keyframe_; }
    // A keyframe newer than the last delivered frame is arriving right now.
    bool   keyframe_in_flight(int64_t now_us) const;
    // The owner should (re)send a keyframe request: one is needed and none is
    // already on its way. The owner paces the requests.
    bool   should_request_keyframe(int64_t now_us) const {
        return needs_keyframe() && !keyframe_in_flight(now_us);
    }
    uint32_t last_delivered() const { return last_delivered_; }

private:
    struct Held {
        VideoFrameHeader hdr;
        std::vector<uint8_t> frame;
        int64_t arrived_us = 0;
    };

    VideoFrameReorderConfig cfg_;
    uint32_t rtt_us_ = 0;
    bool     have_last_ = false;
    bool     awaiting_keyframe_ = true;
    bool     loss_reported_ = false;   // `lost` called for the current recovery
    uint32_t last_delivered_ = 0;
    // Keyed by frame_seq re-based so that std::map ordering matches wrap-safe
    // ordering relative to last_delivered_ (see .cpp).
    std::map<int64_t, Held> held_;
    size_t   held_bytes_ = 0;
    int64_t  gap_since_us_ = 0;   // when we first started waiting for the current gap
    // Newest keyframe whose stream started but has not completed.
    bool     inflight_key_ = false;
    uint32_t inflight_key_seq_ = 0;
    int64_t  inflight_key_since_us_ = 0;
    // Aborted frames ahead of the chain (bounded; pruned as the chain moves).
    std::vector<uint32_t> aborted_;

    void deliver_one(Held&& h, const DeliverFn& deliver);
    void drain(int64_t now_us, const DeliverFn& deliver, const LostFn& lost);
    void require_keyframe(int64_t now_us, const LostFn& lost);
    void report_loss(const LostFn& lost);
    void bound_held();
    void forget_inflight_through(uint32_t seq);
    bool next_is_aborted() const;
    int64_t key_for(uint32_t seq) const;
};

} // namespace parties
