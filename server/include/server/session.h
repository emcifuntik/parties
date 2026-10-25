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
    uint32_t         id = 0;

    HQUIC            quic_connection = nullptr;
    HQUIC            quic_control_stream = nullptr;
    HQUIC            quic_video_stream = nullptr;
    std::mutex       quic_mutex;

    bool             authenticated = false;
    UserId           user_id = 0;
    std::string      username;
    int              role = 3;
    SessionToken     session_token{};
    PublicKey         public_key{};
    uint16_t         protocol_version = protocol::PROTOCOL_VERSION_ASSUMED_LEGACY;
    bool supports_frame_streams() const {
        return protocol::protocol_supports_frame_streams(protocol_version);
    }

    std::atomic<int64_t> video_outstanding_bytes{0};
    std::unordered_map<UserId, ViewerVideoGate> video_gates;
    std::atomic<uint32_t> min_rtt_us{VIDEO_BACKLOG_DEFAULT_RTT_US};
    int64_t rtt_sample_last_us = 0;

    std::atomic<bool> video_ingress_allowed{false};
    std::atomic<uint64_t> video_generation{0};
    std::atomic<int64_t> video_in_buffered_bytes{0};
    std::atomic<int64_t> video_pending_bytes{0};
    static constexpr int64_t VIDEO_INGRESS_MAX_BUFFERED_BYTES = 4 * 4 * 1024 * 1024;

    ChannelId        channel_id = 0;
    bool             muted = false;
    bool             deafened = false;

    uint8_t          share_codec = 0;
    uint16_t         share_width = 0;
    uint16_t         share_height = 0;

    std::unordered_set<UserId> subscribed_sharers;

    std::atomic<bool> alive{true};
};

}
