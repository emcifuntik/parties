#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
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
    // Per-sharer forwarding gate (awaiting-keyframe after a drop). Guarded by
    // Server::subscriptions_mutex_ like subscribed_sharers.
    std::unordered_map<UserId, ViewerVideoGate> video_gates;
    // MinRtt of this connection (QUIC_STATISTICS_V2::MinRtt), sampled on the
    // main loop about once per second and read on the MsQuic thread by the
    // backlog gate. Default until the first sample.
    std::atomic<uint32_t> min_rtt_us{VIDEO_BACKLOG_DEFAULT_RTT_US};

    // ── Video ingress (as a sharer) ──
    // True only while the main loop has this session registered as an active
    // screen sharer (SCREEN_SHARE_START accepted .. stop/leave/disconnect).
    // Per-frame unidirectional streams from a session without it are aborted
    // at PEER_STREAM_STARTED, so unauthenticated or idle peers cannot buffer
    // video on the server.
    std::atomic<bool> video_ingress_allowed{false};
    // Bytes currently buffered across this session's unfinished per-frame
    // ingress streams. Capped at VIDEO_INGRESS_MAX_BUFFERED_BYTES; a stream
    // that would exceed it is aborted.
    std::atomic<int64_t> video_in_buffered_bytes{0};
    static constexpr int64_t VIDEO_INGRESS_MAX_BUFFERED_BYTES = 4 * 4 * 1024 * 1024;  // 16 MB

    // Voice state
    ChannelId        channel_id = 0;     // 0 = not in a channel
    bool             muted = false;
    bool             deafened = false;

    // Screen share metadata (set when sharing, used for late-join notifications)
    uint8_t          share_codec = 0;
    uint16_t         share_width = 0;
    uint16_t         share_height = 0;

    // Subscribe state: the set of sharers whose video streams this viewer is
    // watching (empty = none). A viewer can watch several screen shares at once
    // (the client tiles them in a grid). Mutated only on the server main loop.
    std::unordered_set<UserId> subscribed_sharers;

    // Connection state
    std::atomic<bool> alive{true};
};

} // namespace parties::server
