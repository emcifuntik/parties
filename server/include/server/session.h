#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
#include <parties/datagram_backlog.h>
#include <parties/video_backlog.h>

#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

typedef struct QUIC_HANDLE *HQUIC;

namespace parties::server {

struct Session {
    uint32_t         id = 0;             // Internal session ID

    // QUIC transport
    HQUIC            quic_connection = nullptr;      // QUIC connection handle
    HQUIC            quic_control_stream = nullptr;  // Bidirectional control stream
    HQUIC            quic_video_stream = nullptr;    // Bidirectional video stream
    // Serializes use of the QUIC handles above by data-plane senders against
    // their closure in SHUTDOWN_COMPLETE: senders hold it while calling MsQuic
    // and check `alive`; the shutdown path takes it, sets alive=false and only
    // then closes the handles. Never take sessions_mutex_ while holding this.
    std::mutex       quic_mutex;

    // Authenticated state (set after AUTH_RESPONSE)
    bool             authenticated = false;
    UserId           user_id = 0;
    std::string      username;           // Display name
    int              role = 3;           // Default: User
    SessionToken     session_token{};
    PublicKey         public_key{};
    // Protocol version the client reported in AUTH_IDENTITY. Decides whether
    // this viewer receives video on per-frame unidirectional streams (>= 1.2)
    // or whole frames on the legacy video stream 1.
    uint16_t         protocol_version = protocol::PROTOCOL_VERSION_ASSUMED_LEGACY;
    bool supports_frame_streams() const {
        return protocol::protocol_supports_frame_streams(protocol_version);
    }

    // ── Video backpressure (as a viewer) ──
    // Bytes of video queued + in flight toward this viewer on any path
    // (StreamSend adds, SEND_COMPLETE subtracts). Signed: completion and
    // enqueue can be observed out of order across threads.
    std::atomic<int64_t> video_outstanding_bytes{0};
    // Per-stream forwarding gate (awaiting-keyframe after a drop), keyed by
    // stream id. Guarded by Server::subscriptions_mutex_ like subscribed_sharers.
    std::unordered_map<uint32_t, ViewerVideoGate> video_gates;
    // MinRtt sampled in the connection's QUIC callbacks about once per second.
    // The server loop reads this cache without blocking the QUIC worker.
    std::atomic<uint32_t> min_rtt_us{VIDEO_BACKLOG_DEFAULT_RTT_US};
    int64_t rtt_sample_last_us = 0;  // connection's MsQuic worker only

    // ── Datagrams toward this peer (voice fan-out, screen-share audio, PLI) ──
    // Queued inside MsQuic; media is dropped at the source while the queue is
    // stale so a peer whose path collapsed does not accumulate seconds of
    // voice ahead of its control stream (see parties/datagram_backlog.h).
    // Every speaker in the channel feeds this one queue, hence the larger cap.
    DatagramSendBacklog dgram_backlog{
        DatagramBacklogLimits{ .max_queued = 64, .max_delay_us = 200'000 } };

    // ── Video ingress (as a publisher), one slot per protocol::VideoSource ──
    struct VideoSourceState {
        // True only while the main loop has this source registered as an
        // active stream (SCREEN_SHARE_START accepted .. stop/leave/disconnect).
        // Per-frame streams for a source without it are aborted, and a session
        // with no source allowed has its unidirectional streams refused at
        // PEER_STREAM_STARTED, so idle peers cannot buffer video on the server.
        std::atomic<bool>     ingress_allowed{false};
        std::atomic<uint64_t> generation{0};   // invalidates queued frames on stop
        // Stream metadata for late-join notifications. Main loop only.
        uint8_t  codec  = 0;
        uint16_t width  = 0;
        uint16_t height = 0;
    };
    VideoSourceState video_sources[protocol::VIDEO_SOURCE_COUNT];
    VideoSourceState& video_source(protocol::VideoSource source) {
        return video_sources[static_cast<size_t>(source)];
    }
    const VideoSourceState& video_source(protocol::VideoSource source) const {
        return video_sources[static_cast<size_t>(source)];
    }
    bool video_ingress_allowed(protocol::VideoSource source) const {
        return video_sources[static_cast<size_t>(source)].ingress_allowed.load(
            std::memory_order_acquire);
    }
    bool any_video_ingress_allowed() const {
        for (const auto& s : video_sources)
            if (s.ingress_allowed.load(std::memory_order_acquire)) return true;
        return false;
    }
    bool supports_camera() const {
        return protocol::protocol_supports_camera(protocol_version);
    }
    // Bytes currently buffered across this session's unfinished per-frame
    // ingress streams. Capped at VIDEO_INGRESS_MAX_BUFFERED_BYTES; a stream
    // that would exceed it is aborted.
    std::atomic<int64_t> video_in_buffered_bytes{0};
    // Completed frames waiting for the server loop; bounded separately from
    // unfinished QUIC streams so a busy loop cannot accumulate unlimited video.
    std::atomic<int64_t> video_pending_bytes{0};
    static constexpr int64_t VIDEO_INGRESS_MAX_BUFFERED_BYTES = 4 * 4 * 1024 * 1024;  // 16 MB

    // Voice state
    ChannelId        channel_id = 0;     // 0 = not in a channel
    bool             muted = false;
    bool             deafened = false;

    // Subscribe state: the stream ids (protocol::video_stream_id) this viewer
    // is watching (empty = none). A viewer can watch several streams at once —
    // screen shares and cameras — tiled in a grid. Mutated only on the server
    // main loop.
    std::unordered_set<uint32_t> subscribed_sharers;

    // Connection state
    std::atomic<bool> alive{true};
};

} // namespace parties::server
