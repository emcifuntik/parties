#pragma once

#include <parties/types.h>

#include <string>
#include <cstdint>
#include <atomic>

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

    // Subscribe state: whose video stream this viewer is watching (0 = none)
    UserId           subscribed_sharer = 0;

    // Connection state
    std::atomic<bool> alive{true};
};

} // namespace parties::server
