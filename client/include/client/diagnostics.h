#pragma once

// On-screen diagnostics: the numbers behind the overlay (Ctrl+Shift+D).
//
// DiagnosticsRecorder keeps the last minute of connection and video history
// in fixed 250 ms buckets, and classifies every visible freeze of a watched
// stream by what the frames around it say:
//
//   Sender   the capture times jump as far as the arrivals do: the sharer
//            produced no frames (static screen, game or capture stall, its
//            own congestion control skipping frames).
//   Network  the capture times are continuous but the frames arrived late:
//            they waited in the network (retransmissions, a queue, Wi-Fi).
//   Lost     frames are missing from the sequence: the server dropped them
//            for this viewer (backlog) or the stream was aborted; playback
//            resumed at a keyframe. Also when the frames kept arriving but
//            the decoder had to wait for that keyframe.
//   Decode   frames reached the decoder on time but none reached the screen.
//   Render   our own render loop stalled (GPU busy, a driver or compositor
//            hiccup): the whole window froze, not only the video. Recorded
//            once, for the app (APP_STREAM), not per stream.
//
// A freeze is a gap of FREEZE_US (about six frames at 60 fps). Sender gaps
// count from SENDER_FREEZE_US: a static screen makes the capture deliver
// nothing, and a short pause in a picture that does not change is invisible.
//
// Delivery gaps are measured where frames leave the receive router for the
// decoder (after the reorder and playout buffers, i.e. what the decoder
// gets); presentation gaps where VideoElement receives a decoded frame;
// render stalls between render-loop iterations.
//
// Threading: on_frame_delivered / on_frame_presented / on_render_tick may run
// on any thread; sample and the readers on the main thread. One mutex, never
// held while calling out.
//
// Tests: tests/diagnostics_test.cpp

#include <parties/video_common.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace parties::client {

// What the server reports about its side of our connection (KEEPALIVE_PONG
// link report, see docs/protocol.md). All zero / !valid from older servers.
struct DiagServerReport {
    bool     valid = false;
    uint32_t srtt_us = 0;
    uint32_t loss_permille = 0;          // server -> us
    uint32_t video_outstanding = 0;      // bytes of video queued / in flight to us
    uint32_t video_threshold = 0;        // the backlog budget those are compared with
    uint32_t video_dropped = 0;          // cumulative: frames the backlog gate dropped for us
    uint32_t video_aborted = 0;          // cumulative: queued frame streams aborted for us
};

// Our side of the connection, cumulative counters (NetClient::ConnectionStats).
struct DiagConnectionSample {
    bool     valid = false;
    uint32_t rtt_us = 0;
    uint64_t sent_bytes = 0;
    uint64_t recv_bytes = 0;
    uint64_t sent_packets = 0;
    uint64_t lost_packets = 0;           // suspected minus spurious
    uint32_t congestion_window = 0;
};

class DiagnosticsRecorder {
public:
    static constexpr int64_t  BUCKET_US = 250'000;
    static constexpr size_t   BUCKETS   = 240;              // one minute
    static constexpr int64_t  FREEZE_US = 100'000;          // a gap this long is visible
    static constexpr int64_t  SENDER_FREEZE_US = 200'000;   // see the header comment
    static constexpr size_t   MAX_FREEZES = 32;
    static constexpr uint32_t APP_STREAM = 0;               // Freeze::stream of a render stall

    enum class Series : size_t {
        DownKbps, UpKbps, RttMs, UpLossPct, DownLossPct, ServerRttMs, Count
    };
    enum class FreezeCause { Sender, Network, Lost, Decode, Render };

    struct Freeze {
        uint64_t    id = 0;              // increasing in recording order
        int64_t     at_us = 0;           // when it ended (the frame that resumed playback)
        int64_t     duration_us = 0;
        uint32_t    stream = 0;          // APP_STREAM for a render stall
        FreezeCause cause = FreezeCause::Network;
        uint32_t    frames_lost = 0;     // Lost: how many frame_seq were skipped
    };

    struct StreamSummary {
        uint32_t stream = 0;
        double   fps_presented = 0;      // last second
        double   fps_delivered = 0;      // last second
        double   recv_kbps = 0;          // last second, encoded video
        uint64_t frames_delivered = 0;
        uint64_t frames_presented = 0;
        uint64_t frames_lost = 0;        // frame_seq gaps
        uint64_t keyframes = 0;
        uint32_t freezes_minute = 0;     // in the last minute
        int64_t  longest_freeze_us = 0;  // in the last minute
        int64_t  stalled_us = 0;         // ongoing: since the last presented frame (0 if not stalled)
    };

    // A bucketed history, oldest first.
    using History = std::array<float, BUCKETS>;

    // Any thread. `hdr` is the header of a frame handed to the decoder.
    void on_frame_delivered(uint32_t stream, const VideoFrameHeader& hdr, size_t bytes, int64_t now_us);
    // Any thread. A decoded frame of `stream` reached its video element.
    void on_frame_presented(uint32_t stream, int64_t now_us);
    // Render thread, once per render-loop iteration. drawing = false while
    // nothing is drawn (window minimized): no frame can be presented then, so
    // neither the pause nor the presentation gap it leaves is a freeze.
    void on_render_tick(int64_t now_us, bool drawing);
    // Main thread. Stop tracking a stream (watch closed).
    void forget_stream(uint32_t stream);

    // Main thread, about every BUCKET_US (calls in between are cheap no-ops).
    void sample(int64_t now_us, const DiagConnectionSample& conn, const DiagServerReport& server);

    // Main thread readers.
    History history(Series series) const;
    History stream_fps(uint32_t stream) const;          // presented fps per bucket
    History stream_freezes(uint32_t stream) const;      // 1 where a freeze covered the bucket
    std::vector<StreamSummary> streams(int64_t now_us) const;
    std::vector<Freeze> freezes() const;                // in recording order
    uint64_t revision() const;                          // bumps whenever sample() closes a bucket
    DiagServerReport server_report() const;
    DiagConnectionSample last_connection() const;

    static const char* cause_name(FreezeCause cause);

private:
    struct FrameMark {
        int64_t  at_us = 0;
        uint32_t frame_seq = 0;
        int64_t  capture_ms = 0;         // unwrapped; valid only if has_capture
        bool     has_capture = false;
    };
    // The last noticeable delivery gap (FREEZE_US / 2 or longer), so the
    // presentation gap it causes is classified by it and not counted twice.
    struct DeliveryGap {
        int64_t     end_us = 0;
        int64_t     duration_us = 0;
        FreezeCause cause = FreezeCause::Network;
        bool        recorded = false;
    };
    struct StreamState {
        bool      have_delivery = false;
        FrameMark last_delivery;
        uint32_t  last_capture_raw = 0;
        int64_t   capture_ext_ms = 0;
        int64_t   last_present_us = 0;
        uint64_t  frames_delivered = 0;
        uint64_t  frames_presented = 0;
        uint64_t  frames_lost = 0;
        uint64_t  keyframes = 0;
        uint32_t  lost_since_present = 0;
        DeliveryGap delivery_gap;
        // Per-bucket accumulation (current bucket), then history.
        uint32_t  bucket_presented = 0;
        uint64_t  bucket_bytes = 0;
        bool      bucket_freeze = false;
        History   fps{};
        History   freeze{};
        // Rolling last second (event times) for the summary rates.
        std::deque<int64_t> presented_times;
        std::deque<int64_t> delivered_times;
        std::deque<std::pair<int64_t, size_t>> delivered_bytes;
    };

    void record_freeze(Freeze f);
    static void push(History& h, float v);
    static void trim(std::deque<int64_t>& q, int64_t now_us);

    mutable std::mutex mutex_;
    std::map<uint32_t, StreamState> streams_;
    std::array<History, static_cast<size_t>(Series::Count)> series_{};
    std::deque<Freeze> freezes_;
    uint64_t next_freeze_id_ = 1;
    int64_t bucket_start_us_ = 0;
    uint64_t revision_ = 0;
    DiagConnectionSample last_conn_;
    bool have_conn_ = false;
    DiagServerReport server_;
    // Render loop: the last iteration that drew, and the last stall.
    int64_t last_render_us_ = 0;
    bool    render_paused_ = false;
    int64_t render_stall_end_us_ = 0;
    int64_t render_stall_us_ = 0;
};

} // namespace parties::client
