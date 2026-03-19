#pragma once

#include <cstdint>

namespace parties::protocol {

constexpr uint16_t DEFAULT_PORT = 7800;

// Protocol wire version — increment MINOR for backwards-compatible additions,
// increment MAJOR for breaking changes. Sent in AUTH_IDENTITY.
constexpr uint16_t PROTOCOL_VERSION = 1;

enum class ControlMessageType : uint16_t {
    // Client -> Server
    AUTH_IDENTITY         = 0x0001,  // [protocol_version(2)][pubkey(32)][display_name][timestamp(8)][signature(64)][password]
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
    ADMIN_RENAME_CHANNEL  = 0x0205,

    // Server -> Client admin responses
    ADMIN_RESULT          = 0x0301,

    // ── Text chat ──────────────────────────────────────────────────────

    // Client -> Server
    CHAT_SEND             = 0x0401,  // [channel_id(4)][text(string)][attachment_count(1)][attachments...]
    CHAT_HISTORY_REQ      = 0x0402,  // [channel_id(4)][before_id(8)][limit(2)]
    CHAT_PIN              = 0x0403,  // [message_id(8)]
    CHAT_UNPIN            = 0x0404,  // [message_id(8)]
    CHAT_DELETE           = 0x0405,  // [message_id(8)]
    CHAT_FILE_UPLOAD_REQ  = 0x0406,  // [message_id(8)][file_index(1)][file_size(8)]
    CHAT_FILE_DOWNLOAD_REQ= 0x0407,  // [file_id(8)]
    CHAT_SEARCH           = 0x0408,  // [channel_id(4)][query(string)][before_id(8)][limit(2)]
    CHAT_PINNED_REQ       = 0x0409,  // [channel_id(4)]

    // Server -> Client
    CHAT_MESSAGE          = 0x0501,  // [msg_id(8)][channel_id(4)][sender_id(4)][sender_name][timestamp(8)][text][pinned(1)][attachments...]
    CHAT_HISTORY_RESP     = 0x0502,  // [channel_id(4)][has_more(1)][count(2)][messages...]
    CHAT_MESSAGE_DELETED  = 0x0503,  // [message_id(8)][channel_id(4)]
    CHAT_FILE_UPLOAD_RESP = 0x0504,  // [message_id(8)][file_index(1)][accepted(1)][reason(string)]
    CHAT_FILE_READY       = 0x0505,  // [message_id(8)][file_index(1)][file_id(8)]
    CHAT_SEARCH_RESP      = 0x0506,  // [channel_id(4)][count(2)][messages...]
    CHAT_PINNED_RESP      = 0x0507,  // [channel_id(4)][count(2)][messages...]
    CHAT_CHANNEL_LIST     = 0x0508,  // [count(4)][channel_id(4), name(string), sort_order(4)]...

    // Admin text channels (client -> server)
    ADMIN_CREATE_TEXT_CHANNEL = 0x040A,  // [name(string)]
    ADMIN_DELETE_TEXT_CHANNEL = 0x040B,  // [channel_id(4)]
};

// Data plane packet types (first byte of every datagram/stream)
constexpr uint8_t VOICE_PACKET_TYPE        = 0x01;
constexpr uint8_t VIDEO_FRAME_PACKET_TYPE  = 0x02;
constexpr uint8_t VIDEO_CONTROL_TYPE       = 0x03;
constexpr uint8_t STREAM_AUDIO_PACKET_TYPE = 0x04;  // Screen share audio (Opus, stereo)

// File transfer stream type bytes (first byte on streams 2+)
constexpr uint8_t STREAM_TYPE_FILE_UPLOAD   = 0x10;
constexpr uint8_t STREAM_TYPE_FILE_DOWNLOAD = 0x11;

// Video control subtypes
constexpr uint8_t VIDEO_CTL_PLI         = 0x01;
constexpr uint8_t VIDEO_CTL_SHARE_START = 0x02;
constexpr uint8_t VIDEO_CTL_SHARE_STOP  = 0x03;

} // namespace parties::protocol
