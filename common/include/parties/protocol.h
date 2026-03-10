#pragma once

#include <cstdint>

namespace parties::protocol {

constexpr uint16_t DEFAULT_PORT = 7800;

enum class ControlMessageType : uint16_t {
    // Client -> Server
    AUTH_IDENTITY         = 0x0001,  // [pubkey(32)][display_name][timestamp(8)][signature(64)]
    CHANNEL_JOIN          = 0x0002,
    CHANNEL_LEAVE         = 0x0003,
    KEEPALIVE_PING        = 0x0004,
    VOICE_STATE_UPDATE    = 0x0005,
    SCREEN_SHARE_START    = 0x0007,
    SCREEN_SHARE_STOP     = 0x0008,
    SCREEN_SHARE_VIEW     = 0x0009,   // Subscribe to a sharer's stream [target_user_id(4)], 0 = unsubscribe
    SCREEN_SHARE_UPDATE   = 0x000A,   // Update share metadata: [codec(1)][width(2)][height(2)]

    // Server -> Client
    AUTH_RESPONSE         = 0x0101,
    CHANNEL_LIST          = 0x0102,
    CHANNEL_USER_LIST     = 0x0103,
    USER_JOINED_CHANNEL   = 0x0104,
    USER_LEFT_CHANNEL     = 0x0105,
    USER_VOICE_STATE      = 0x0106,
    USER_ROLE_CHANGED     = 0x0108,   // [user_id(4)][new_role(1)]
    KEEPALIVE_PONG        = 0x0107,
    CHANNEL_KEY           = 0x0109,
    SCREEN_SHARE_STARTED  = 0x010A,
    SCREEN_SHARE_STOPPED  = 0x010B,
    SCREEN_SHARE_DENIED   = 0x010C,
    SERVER_ERROR          = 0x01FF,

    // Admin operations (client -> server)
    ADMIN_CREATE_CHANNEL  = 0x0201,
    ADMIN_DELETE_CHANNEL  = 0x0202,
    ADMIN_SET_ROLE        = 0x0203,
    ADMIN_KICK_USER       = 0x0204,

    // Server -> Client admin responses
    ADMIN_RESULT          = 0x0301,
};

// Data plane packet types (first byte of every datagram/stream)
constexpr uint8_t VOICE_PACKET_TYPE        = 0x01;
constexpr uint8_t VIDEO_FRAME_PACKET_TYPE  = 0x02;
constexpr uint8_t VIDEO_CONTROL_TYPE       = 0x03;
constexpr uint8_t STREAM_AUDIO_PACKET_TYPE = 0x04;  // Screen share audio (Opus, stereo)

// Video control subtypes
constexpr uint8_t VIDEO_CTL_PLI         = 0x01;
constexpr uint8_t VIDEO_CTL_SHARE_START = 0x02;
constexpr uint8_t VIDEO_CTL_SHARE_STOP  = 0x03;

} // namespace parties::protocol
