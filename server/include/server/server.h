#pragma once

#include <server/config.h>
#include <server/quic_server.h>
#include <server/database.h>
#include <parties/types.h>
#include <parties/video_common.h>
#include <parties/video_backlog.h>
#include <parties/video_frame_reorder.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace parties::server {

class Server {
public:
    Server();
    ~Server();

    // Initialize with config, start listening
    bool start(const Config& cfg);

    // Run one iteration of the main loop
    void run();

    // Signal the server to stop
    void stop();

private:
    void process_control_messages();
    void process_data_packets();
    void enqueue_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void process_video_frames();
    void process_file_transfers();
    void process_disconnects();
    void handle_message(const IncomingMessage& msg);
    // Broadcast USER_LEFT and clean up screen-share state for a session that
    // dropped. Runs on the main loop only (see SessionDisconnect queue).
    void process_disconnect(uint32_t session_id, UserId user_id, ChannelId channel_id);

    void send_error(uint32_t session_id, const std::string& message,
                    protocol::ServerErrorCode code = protocol::ServerErrorCode::Generic);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);

    // Screen sharing
    // Runs on the server loop after ingress reordering. `data` = [14-byte
    // VideoFrameHeader][encoded] from either input path. Applies the per-viewer
    // backlog gate (ViewerVideoGate + video_backlog_threshold_bytes of the
    // sharer's measured rate), forwards whole frames on each viewer's path via
    // quic_.send_video_frame_to, and asks the sharer for a keyframe (coalesced)
    // when a throttled viewer can resume.
    void forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void forward_stream_audio(const DataPacket& pkt);
    // Main loop. First statement: per-sender token bucket (4/s, burst 4).
    // PLIs from a viewer currently over its backlog threshold are not
    // forwarded; forwarded PLIs are coalesced per sharer (one per 200 ms).
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);
    // Send [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][requester u32] to `sharer_user_id`
    // through the per-sharer coalescer. requester = 0 when server-originated.
    // Callable from the MsQuic thread and the main loop (guarded internally).
    void request_keyframe_from_sharer(UserId sharer_user_id, UserId requester);
    // SCREEN_SHARE_VIEWER to the sharer (skipped when viewer == sharer or the
    // sharer runs a pre-1.2 client). Main loop.
    void notify_viewer_change(UserId sharer_user_id, UserId viewer_user_id, bool watching);
    // user_id -> session_id index maintained on the main loop (auth success,
    // identity takeover, disconnect). Replaces get_sessions() scans on the
    // control paths.
    std::shared_ptr<Session> session_for_user(UserId user_id);
    // Drop every subscription of `session` (subscribed_sharers + video_gates,
    // under subscriptions_mutex_) and, when `notify`, tell each sharer it
    // lost this viewer (SCREEN_SHARE_VIEWER watching=0). Main loop.
    void clear_viewer_subscriptions(const std::shared_ptr<Session>& session, bool notify);

    Config config_;
    Database db_;
    QuicServer quic_;
    std::atomic<bool> running_{false};

    struct PendingVideoFrame {
        std::shared_ptr<Session> session;
        uint64_t generation;
        std::vector<uint8_t> data;
    };
    ThreadQueue<PendingVideoFrame> video_incoming_;
    struct VideoIngress {
        std::shared_ptr<Session> session;
        VideoFrameReorderBuffer reorder;
    };
    // Server-loop only. Reorder before gating or forwarding to either protocol
    // version, since legacy viewers have no reorder buffer of their own.
    std::unordered_map<uint32_t, VideoIngress> video_ingress_;

    // Screen share state: channel_id -> set of sharer user_ids
    std::mutex sharers_mutex_;
    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;

    // Guards subscriptions and per-viewer gates during server-loop forwarding
    // and subscription changes.
    std::mutex subscriptions_mutex_;

    // Per-sharer forwarding state (keyed by sharer user_id). Guarded by
    // video_state_mutex_; forwarding, PLI and stop run on the server loop. Erased in stop_screen_share.
    struct SharerVideoState {
        RateEstimator rate{1.0};            // bytes/s forwarded (drives the backlog threshold)
        Coalescer     pli{200'000};         // one forwarded/originated PLI per 200 ms
        uint32_t      last_frame_seq = 0;
        bool          have_frame = false;
        Coalescer     drop_log{1'000'000};  // rate-limits the per-sharer drop log line
        uint32_t      drops_since_log = 0;  // whole-frame drops (all viewers) since the last line
        // Largest keyframe seen recently (bytes) — headroom added to every
        // viewer's backlog budget so one admitted keyframe cannot trip the
        // gate for the deltas behind it. Decays: reset to the newest keyframe
        // size when it is older than ~10 s.
        int64_t       max_keyframe_bytes = 0;
        int64_t       max_keyframe_at_us = 0;
    };
    std::mutex video_state_mutex_;
    std::unordered_map<UserId, SharerVideoState> sharer_video_state_;

    // Per-viewer backlog budget for one frame: sum of the rates of every sharer
    // the viewer is subscribed to (they share one outstanding-bytes counter)
    // x (viewer MinRtt + 250 ms) + keyframe headroom. Caller holds
    // video_state_mutex_ and subscriptions_mutex_ (see forward_video_frame).
    int64_t viewer_backlog_threshold(const Session& viewer,
                                     const std::unordered_map<UserId, double>& sharer_rates,
                                     int64_t keyframe_headroom) const;


    // Per-session control-message rate limit (video control datagrams).
    // Main loop only.
    std::unordered_map<uint32_t, TokenBucket> video_control_buckets_;

    // user_id -> session_id. Main loop only (see session_for_user).
    std::mutex user_index_mutex_;
    std::unordered_map<UserId, uint32_t> user_sessions_;

    // session_id -> Session for every authenticated session. QuicServer erases
    // a dropped session from its map before the main loop learns about it, so
    // this keeps the object reachable for process_disconnect (subscription
    // cleanup + viewer notifications). Main loop only.
    std::unordered_map<uint32_t, std::shared_ptr<Session>> authed_sessions_;

    // Auth replay guard: recently-accepted (pubkey, timestamp) pairs. Drops an
    // AUTH_IDENTITY whose signed blob we've already seen inside the freshness
    // window, defeating replay of a captured handshake. Pruned by timestamp.
    std::unordered_map<Fingerprint, uint64_t> recent_auth_;
};

} // namespace parties::server
