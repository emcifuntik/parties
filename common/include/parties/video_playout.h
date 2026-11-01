#pragma once

// Playout (jitter) clock for one received video stream.
//
// Frames of a reliable per-frame stream arrive complete but unevenly: one
// that lost a packet waits for its retransmission and the frames behind it
// pile up and arrive together. Handed to the decoder as they arrive, a clump
// shows as a stall followed by a jump (the renderer can only show the newest
// frame). Presented at their capture spacing instead, behind a small delay
// that covers the jitter, motion stays smooth.
//
// The sender stamps VideoFrameHeader::timestamp with its capture time in
// milliseconds (any monotonic clock, wrapping) and sets VIDEO_FLAG_CAPTURE_TIME.
// For each frame, transit = arrival - capture (sender and receiver clocks
// differ by an unknown constant, which cancels out). Over a sliding window:
//   base   = the smallest transit (the fastest a frame made it)
//   jitter = the given percentile of (transit - base)
// The playout delay follows the jitter: up at once, down slowly, at most
// max_delay_ms. A frame plays at capture + base + delay, or on arrival if
// that moment has already passed. Frames without the flag (older senders)
// play on arrival.
//
// Not thread-safe; one per stream, fed in decode order.
//
// Tests: tests/video_receive_router_test.cpp

#include <parties/video_common.h>

#include <cstdint>
#include <deque>

namespace parties {

struct VideoPlayoutConfig {
    uint32_t max_delay_ms   = 300;    // never delay a frame more than this
    uint32_t window_ms      = 3000;   // transit history
    double   percentile     = 0.95;   // of (transit - base)
    uint32_t decay_ms_per_s = 100;    // how fast the delay comes back down
};

class VideoPlayoutClock {
public:
    explicit VideoPlayoutClock(const VideoPlayoutConfig& cfg = {}) : cfg_(cfg) {}

    // Local time (same clock as arrival_us) at which to present this frame.
    int64_t playout_time(const VideoFrameHeader& hdr, int64_t arrival_us);

    int64_t delay_us() const { return delay_us_; }
    void reset();

private:
    struct Sample { int64_t at_us; int64_t transit_us; };

    VideoPlayoutConfig cfg_;
    bool     have_ts_ = false;
    uint32_t last_ts_ = 0;
    int64_t  last_ext_ms_ = 0;
    int64_t  delay_us_ = 0;
    int64_t  last_update_us_ = 0;
    std::deque<Sample> samples_;
};

} // namespace parties
