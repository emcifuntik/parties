#pragma once

#include <parties/types.h>

#include <string>
#include <cstdint>
#include <atomic>
#include <unordered_set>

typedef struct QUIC_HANDLE *HQUIC;

namespace parties::server {

struct Session {
    uint32_t         id = 0;             // Internal session ID

    // QUIC transport
    HQUIC            quic_connection = nullptr;      // QUIC connection handle
    HQUIC            quic_control_stream = nullptr;  // Bidirectional control stream
    HQUIC            quic_video_stream = nullptr;    // Bidirectional video stream

    // Authenticated state (set after AUTH_RESPONSE)
    bool             authenticated = false;
    UserId           user_id = 0;
    std::string      username;           // Display name
    int              role = 3;           // Default: User
    SessionToken     session_token{};
    PublicKey         public_key{};

    // Voice state
    ChannelId        channel_id = 0;     // 0 = not in a channel
    bool             muted = false;
    bool             deafened = false;

    // Screen share metadata (set when sharing, used for late-join notifications)
    uint8_t          share_codec = 0;
    uint16_t         share_width = 0;
    uint16_t         share_height = 0;

    // Webcam metadata (set when streaming a camera, for late-join notifications)
    uint8_t          camera_codec = 0;
    uint16_t         camera_width = 0;
    uint16_t         camera_height = 0;

    // Subscribe state: the set of sharers whose video streams this viewer is
    // watching (empty = none). A viewer can watch several screen shares at once
    // (the client tiles them in a grid). Mutated only on the server main loop.
    std::unordered_set<UserId> subscribed_sharers;

    // Camera streamers this viewer is watching. Separate from subscribed_sharers
    // so a viewer can watch any mix of screens and cameras.
    std::unordered_set<UserId> subscribed_camera_sharers;

    // Connection state
    std::atomic<bool> alive{true};
};

} // namespace parties::server
