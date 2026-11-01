#pragma once

// Sharer-side congestion control for screen-share video.
//
// Two coupled controls, both required:
//   * Admission (latency control): a capture frame is encoded only when the
//     bytes still queued/in flight toward the server would not add more than a
//     small queueing delay on top of the RTT. Skipping at capture keeps the
//     encoder's reference chain intact (frame_seq is stamped after encode), so
//     a skipped frame costs nothing and needs no keyframe.
//   * Bitrate adaptation (bandwidth control): with a rate-controlled encoder,
//     halving the input fps roughly doubles the per-frame size, so admission
//     alone cannot reduce bandwidth. A STANDING queue — the window's minimum
//     queueing delay stays high, i.e. the link never drained — or a high
//     share of admission drops lowers the bitrate, straight to just below the
//     goodput measured in that window (bytes acknowledged per second) when
//     that is lower than the multiplicative step; a clean second raises it
//     again toward the user's target.
//     Deliberately NOT congestion signals: packet loss (the transport runs BBR
//     and retransmits; random Wi-Fi/mobile loss says nothing about capacity,
//     and treating it as congestion pinned streams at the floor), a single
//     admission drop and a short queue spike (a keyframe drains by itself).
//     Loss is still used to know how much unacknowledged data is normal: each
//     lost packet costs about one more round trip before it is acknowledged.
//
// Threading contract (must hold):
//   * Accounting entry points (on_enqueued / on_completed / on_send_failed /
//     on_encoder_drop) and admit_frame() are lock-free and may be called from
//     any thread (MsQuic worker, WGC capture thread, encode thread).
//   * set_target_bitrate, on_connection_stats, tick, reset and stats are
//     main-thread only: all AIMD/window state lives there and is mutated
//     solely by tick(). tick() publishes the adapted bitrate and the admission
//     allowance through atomics.
//   * take_bitrate_update / take_keyframe_request are consumed by the encode
//     thread (atomic exchange).
//
// Implementation: common/src/video_send_controller.cpp
// Tests:          tests/video_send_controller_test.cpp

#include <parties/video_common.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace parties {

struct VideoSendControllerConfig {
    uint32_t target_bitrate_bps   = VIDEO_DEFAULT_BITRATE;   // user ceiling
    uint32_t min_bitrate_bps      = VIDEO_ADAPT_MIN_BITRATE;
    uint32_t queue_budget_ms      = 120;   // admission: max queueing delay beyond the RTT
    // Standing queue (window minimum) that counts as congestion. Measured
    // beyond the expected in-flight time, so it still contains the peer's ACK
    // delay (MsQuic: up to 25 ms) and the send granularity (a frame is queued
    // whole), which never drain: a smaller value reads a healthy link as
    // congested, at any bitrate, and walks the stream down to the floor.
    uint32_t congestion_queue_ms  = 100;
    double   admission_drop_ratio = 0.2;   // share of frames skipped by admission that counts as congestion
    uint32_t window_ms            = 500;   // evaluation window
    uint32_t congested_windows    = 2;     // consecutive congested windows before a decrease
    uint32_t clean_windows        = 2;     // consecutive clean windows (1 s) before an increase
    double   decrease_factor      = 0.85;
    double   goodput_factor       = 0.9;   // a decrease goes at least this far below the measured goodput
    double   increase_factor      = 1.10;
    uint32_t default_rtt_us       = 50'000;   // used until the first connection stats arrive
    uint32_t min_reconfigure_ms   = 500;   // never publish bitrate updates faster than this
    double   min_reconfigure_delta = 0.10; // ...or smaller than 10 %
};

class VideoSendController {
public:
    VideoSendController();
    explicit VideoSendController(const VideoSendControllerConfig& cfg);

    // Main thread. Share start: re-seeds the config, the adapted bitrate (=
    // target, force-published so the encoder starts at the right rate), the
    // AIMD windows and the per-share counters. Does NOT touch the outstanding
    // byte count: sends of a previous share may still be in flight on the same
    // connection and their SEND_COMPLETEs must keep matching what was added.
    void reset(const VideoSendControllerConfig& cfg);
    void reset();
    // Main thread. Connection gone: MsQuic has completed or canceled every
    // send, so the outstanding byte count is meaningless and is zeroed.
    void clear_outstanding();

    // Main thread. Raises/lowers the ceiling; the adapted bitrate is clamped to it
    // immediately (a lower ceiling takes effect on the next take_bitrate_update()).
    void set_target_bitrate(uint32_t bps);

    // ── Lock-free accounting (any thread) ───────────────────────────────────
    void on_enqueued(size_t bytes);      // after a successful StreamSend / send call
    void on_completed(size_t bytes);     // QUIC SEND_COMPLETE (data acknowledged); feeds the goodput
    void on_send_failed(size_t bytes);   // send call failed synchronously: nothing queued, frame lost
    void on_encoder_drop();              // encoder could not accept a frame (e.g. AMF_INPUT_FULL)
    int64_t outstanding_bytes() const;   // queued + in flight, never negative

    // Capture-thread admission. true = encode this frame; false = skip it (counted
    // as an admission drop for AIMD). Atomics only; never blocks.
    bool admit_frame(int64_t now_us);

    // ── Main thread ─────────────────────────────────────────────────────────
    // Absolute (monotonic) counters from QUIC_PARAM_CONN_STATISTICS_V2; deltas
    // are computed internally. Call once per tick when available. rtt_us is
    // the SMOOTHED RTT: the controller keeps its own windowed minimum of it
    // (about 10 s) as the base RTT. QUIC's MinRtt is a lifetime minimum: it
    // keeps a value from the handshake or from before a path change forever,
    // and every byte in flight on the longer path then looks like queueing.
    void on_connection_stats(int64_t now_us, uint32_t rtt_us,
                             uint64_t suspected_lost_packets,
                             uint64_t spurious_lost_packets,
                             uint64_t sent_packets);

    // Runs the AIMD window logic and refreshes the admission allowance. Cheap;
    // call every tick.
    void tick(int64_t now_us);

    // Current adapted bitrate (atomic, any thread).
    uint32_t bitrate_bps() const;

    // ── Encode thread ───────────────────────────────────────────────────────
    // Returns the new encoder bitrate once after each change (rate-limited by
    // min_reconfigure_ms / min_reconfigure_delta in tick()).
    std::optional<uint32_t> take_bitrate_update();
    // True once after a send failure: the next encoded frame must be a keyframe
    // because viewers lost a reference frame.
    bool take_keyframe_request();

    struct Stats {
        uint32_t bitrate_bps        = 0;
        int64_t  outstanding_bytes  = 0;
        uint32_t rtt_us             = 0;
        uint32_t queue_delay_ms     = 0;   // last computed queueing delay beyond the expected in-flight time
        uint32_t goodput_bps        = 0;   // last window
        double   loss_ratio         = 0.0; // last window
        uint32_t admission_drops    = 0;   // cumulative since reset
        uint32_t send_failures      = 0;   // cumulative since reset
        uint32_t decreases          = 0;   // cumulative AIMD decreases since reset
    };
    Stats stats() const;   // main thread

private:
    // Queueing delay (ms) implied by `outstanding` at `bitrate` once the
    // in-flight share (bitrate * rtt) is removed. Pure helper, used by tests.
public:
    static uint32_t queue_delay_ms(int64_t outstanding_bytes, uint32_t bitrate_bps, uint32_t rtt_us);
    // How long sent data normally stays unacknowledged: one RTT, plus the
    // retransmission wait of the frames that lost a packet. A frame of a few
    // packets loses one with probability ~packets x loss, and then waits about
    // one more RTT (more on a tail loss), so even 3 % loss adds most of an RTT
    // to the outstanding data. Capped at 2.5 RTTs.
    static uint32_t in_flight_time_us(uint32_t rtt_us, double loss_ratio);

private:
    VideoSendControllerConfig cfg_;

    // Lock-free face
    std::atomic<int64_t>  outstanding_{0};
    std::atomic<int64_t>  allowance_bytes_{0};      // admit while outstanding_ <= allowance
    std::atomic<uint32_t> bitrate_bps_{0};
    std::atomic<uint32_t> pending_bitrate_{0};      // 0 = nothing pending
    std::atomic<uint32_t> admission_drops_{0};
    std::atomic<uint32_t> admission_drops_window_{0};
    std::atomic<uint32_t> admitted_window_{0};
    std::atomic<int64_t>  completed_bytes_window_{0};
    std::atomic<uint32_t> send_failures_{0};
    std::atomic<uint32_t> encoder_drops_window_{0};
    std::atomic<bool>     keyframe_request_{false};

    // Main-thread state
    uint32_t rtt_us_ = 0;              // base RTT: windowed minimum of the samples
    uint32_t rtt_block_min_us_ = 0;    // minimum of the current block (0 = none)
    uint32_t rtt_prev_block_min_us_ = 0;
    int64_t  rtt_block_start_us_ = 0;
    int64_t  window_start_us_ = 0;
    uint32_t congested_windows_ = 0;
    uint32_t clean_windows_ = 0;
    uint32_t last_queue_delay_ms_ = 0;
    uint32_t peak_queue_delay_ms_ = 0;   // within the current window
    uint32_t min_queue_delay_ms_ = 0;    // within the current window (standing queue)
    bool     queue_sampled_ = false;     // min_queue_delay_ms_ holds a sample
    uint32_t last_goodput_bps_ = 0;
    double   last_loss_ratio_ = 0.0;
    bool     have_stats_ = false;
    uint64_t last_suspected_lost_ = 0;
    uint64_t last_spurious_lost_ = 0;
    uint64_t last_sent_packets_ = 0;
    uint64_t window_lost_ = 0;
    uint64_t window_sent_ = 0;
    uint32_t published_bitrate_ = 0;
    int64_t  last_publish_us_ = 0;
    uint32_t decreases_ = 0;

    void publish_bitrate_if_needed(int64_t now_us, bool force);
    void refresh_allowance();
};

} // namespace parties
