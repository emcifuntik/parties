#pragma once

// Viewer side of the per-frame video streams: one VideoFrameReorderBuffer per
// watched stream, plus the keyframe-request (PLI) policy.
//
// Lifecycle: a stream is open() when the viewer starts watching it and
// close()d when it stops. Frames of a stream that is not open are dropped, so
// a buffer can never outlive its watch and greet a restarted share (whose
// frame counter starts over) with an old baseline.
//
// Keyframe requests are paced per stream: a request goes out at most every
// `backoff`, which starts at base_backoff_ms and doubles up to max_backoff_ms
// while the stream stays broken, and drops back once a keyframe is
// delivered. Nothing is requested while a newer keyframe is already arriving
// (its stream header was seen). Under heavy loss a big keyframe can take
// several round trips: re-asking every 500 ms would force a new keyframe for
// every viewer before the previous one ever landed.
//
// Playout: frames leave the reorder buffer in decode order and are handed to
// `deliver` at the time their VideoPlayoutClock picks (capture spacing behind
// a jitter-sized delay), from the router's playout thread. With
// cfg.playout = false, `deliver` runs inline instead (tests).
//
// Threading: on_frame_started / on_frame / on_frame_aborted run on the
// transport thread; the rest on the owner's main thread; `deliver` on the
// playout thread, under none of our mutexes (inline mode: under the buffers
// mutex). Mutex order: buffers -> playout, buffers -> pacing. `send_request`
// may run under the buffers mutex (a gap found while a frame is processed).
// So callbacks may call back into this class only through request_keyframe(),
// which takes nothing but the pacing mutex.
//
// Implementation: common/src/video_receive_router.cpp
// Tests:          tests/video_receive_router_test.cpp, tests/video_network_test.cpp

#include <parties/video_frame_reorder.h>
#include <parties/video_playout.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace parties {

struct VideoReceiveRouterConfig {
    VideoFrameReorderConfig reorder;
    uint32_t base_backoff_ms = 500;
    uint32_t max_backoff_ms  = 4000;
    bool     playout = true;            // false: deliver inline, unpaced
    VideoPlayoutConfig playout_clock;
};

class VideoReceiveRouter {
public:
    // A frame in decode order for `stream`: [14-byte header][encoded].
    using DeliverFn = std::function<void(uint32_t stream, const VideoFrameHeader& hdr,
                                         std::vector<uint8_t>&& frame)>;
    // Put a keyframe request for `stream` on the wire (already paced).
    using SendRequestFn = std::function<void(uint32_t stream)>;

    VideoReceiveRouter(DeliverFn deliver, SendRequestFn send_request,
                       const VideoReceiveRouterConfig& cfg = {});
    ~VideoReceiveRouter();
    VideoReceiveRouter(const VideoReceiveRouter&) = delete;
    VideoReceiveRouter& operator=(const VideoReceiveRouter&) = delete;

    // ── Watch lifecycle (main thread) ───────────────────────────────────────
    // Starts (or restarts, resetting it) a stream. With now_us, the first
    // keyframe request waits one base backoff: subscribing already makes the
    // server ask the sharer for a keyframe.
    void open(uint32_t stream, int64_t now_us = 0);
    void close(uint32_t stream);
    void close_all();
    bool is_open(uint32_t stream) const;

    // Smoothed RTT of the connection the frames arrive on (0 = unknown).
    void set_rtt(uint32_t rtt_us);

    // ── Transport thread ────────────────────────────────────────────────────
    void on_frame_started(uint32_t stream, const VideoFrameHeader& hdr, int64_t now_us);
    void on_frame(uint32_t stream, std::vector<uint8_t>&& frame, int64_t now_us);
    // have_seq = false: the stream died before its header arrived, so which
    // frame is gone is unknown; only a keyframe can help.
    void on_frame_aborted(uint32_t stream, uint32_t frame_seq, bool have_seq, int64_t now_us);

    // ── Any thread ──────────────────────────────────────────────────────────
    // A consumer (decoder) lost its reference: ask for a keyframe, paced.
    void request_keyframe(uint32_t stream, int64_t now_us);

    // ── Main thread ─────────────────────────────────────────────────────────
    // Expires reorder gaps and re-sends keyframe requests (paced) for every
    // open stream that still needs one. `decoder_needs_keyframe`, when set,
    // adds streams whose decoder waits for a keyframe the buffer already
    // delivered past (e.g. a decode discontinuity).
    void tick(int64_t now_us, const std::function<bool(uint32_t stream)>& decoder_needs_keyframe = {});

    // True while `stream` has no usable reference and none is arriving.
    bool should_request_keyframe(uint32_t stream, int64_t now_us) const;

    struct Stats {
        uint64_t frames_delivered = 0;
        uint64_t keyframes_delivered = 0;
        uint64_t requests_sent = 0;
        uint64_t requests_paced = 0;    // suppressed by the backoff
        int64_t  playout_delay_us = 0;  // current jitter cushion
    };
    Stats stats(uint32_t stream) const;

private:
    struct Pacing {
        int64_t next_us = 0;
        int64_t backoff_us = 0;
    };
    struct StreamState {
        VideoFrameReorderBuffer buffer;
        VideoPlayoutClock clock;
        Stats stats;
    };
    struct Pending {
        int64_t at_us;
        VideoFrameHeader hdr;
        std::vector<uint8_t> frame;
    };

    VideoReceiveRouterConfig cfg_;
    DeliverFn deliver_;
    SendRequestFn send_request_;

    mutable std::mutex buffers_mutex_;
    std::unordered_map<uint32_t, StreamState> streams_;
    uint32_t rtt_us_ = 0;

    mutable std::mutex pacing_mutex_;
    std::unordered_map<uint32_t, Pacing> pacing_;
    std::unordered_map<uint32_t, Stats> pacing_stats_;

    // Playout queue, per stream in decode order.
    std::mutex playout_mutex_;
    std::condition_variable playout_cv_;
    std::unordered_map<uint32_t, std::deque<Pending>> pending_;
    bool playout_stop_ = false;
    std::thread playout_thread_;
    void playout_loop();

    VideoFrameReorderBuffer::DeliverFn deliver_fn(uint32_t stream, StreamState& state, int64_t now_us);
    VideoFrameReorderBuffer::LostFn lost_fn(uint32_t stream, int64_t now_us);
    void keyframe_arrived(uint32_t stream);
};

} // namespace parties
