#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace parties::protocol {

constexpr uint16_t DEFAULT_PORT = 7800;

// Protocol wire version. Increment MINOR for backwards-compatible additions
// (new optional message types/fields), MAJOR for breaking changes. The two are
// packed into one u16 (major in the high byte, minor in the low byte) and sent
// in AUTH_IDENTITY. The server rejects only on a MAJOR mismatch, so a minor
// bump doesn't lock out older clients.
constexpr uint8_t  PROTOCOL_VERSION_MAJOR = 1;
constexpr uint8_t  PROTOCOL_VERSION_MINOR = 2;
constexpr uint16_t PROTOCOL_VERSION =
    (static_cast<uint16_t>(PROTOCOL_VERSION_MAJOR) << 8) | PROTOCOL_VERSION_MINOR;

constexpr uint8_t protocol_major(uint16_t version) { return static_cast<uint8_t>(version >> 8); }
constexpr uint8_t protocol_minor(uint16_t version) { return static_cast<uint8_t>(version & 0xFF); }

// Version a peer must report before we send it video on per-frame streams.
// A peer that never reported a version (pre-1.2 server or client) is assumed
// to be 1.1 and keeps receiving whole frames on the legacy reliable video
// stream (stream 1).
constexpr uint16_t PROTOCOL_VERSION_ASSUMED_LEGACY = (1u << 8) | 1u;
constexpr uint16_t PROTOCOL_VERSION_FRAME_STREAMS  = (1u << 8) | 2u;
constexpr bool protocol_supports_frame_streams(uint16_t version) {
    return protocol_major(version) == 1 && version >= PROTOCOL_VERSION_FRAME_STREAMS;
}

enum class VideoTransport : uint8_t { LegacyStream1, PerFrameStreams };
constexpr VideoTransport video_transport_for(uint16_t peer_protocol_version) {
    return protocol_supports_frame_streams(peer_protocol_version) ? VideoTransport::PerFrameStreams
                                                                  : VideoTransport::LegacyStream1;
}

// SERVER_ERROR payload: [code(u16)][message(string)]. The code lets the client
// react programmatically (retry vs. update vs. give up) instead of parsing the
// human-readable string.
enum class ServerErrorCode : uint16_t {
    Generic          = 0,
    BadVersion       = 1,   // incompatible protocol major
    BadAuth          = 2,   // malformed message / bad signature / stale timestamp
    BadPassword      = 3,
    Kicked           = 4,   // removed by an admin
    Replaced         = 5,   // a newer login for the same identity took over
    RateLimited      = 6,
    NotFound         = 7,
    TooLarge         = 8,
    PermissionDenied = 9,
    Internal         = 10,
};

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
    RETIRED_CHANNEL_KEY   = 0x0109,
    SCREEN_SHARE_STARTED  = 0x010A,  // [sharer_id(4)][codec(1)][width(2)][height(2)][replay(1), optional since 1.2:
                                      //  1 = late-join replay of an already active share, 0/absent = live start]
    SCREEN_SHARE_STOPPED  = 0x010B,
    SCREEN_SHARE_DENIED   = 0x010C,
    SCREEN_SHARE_VIEWER   = 0x010D,   // To the sharer only (1.2+): [viewer_id(4)][watching(1)] — a viewer
                                      // subscribed (1) to / unsubscribed (0) from your stream. Never sent
                                      // for the sharer's own self-preview subscription.
    SERVER_ERROR          = 0x01FF,

    // Admin operations (client -> server)
    ADMIN_CREATE_CHANNEL  = 0x0201,
    ADMIN_DELETE_CHANNEL  = 0x0202,
    ADMIN_SET_ROLE        = 0x0203,
    ADMIN_KICK_USER       = 0x0204,
    ADMIN_RENAME_CHANNEL  = 0x0205,

    // Server -> Client admin responses
    ADMIN_RESULT          = 0x0301,

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

// Every enumerator is a case label, so reusing a value (retired ones included)
// is a duplicate-case error and a missing case is a -Wswitch error.
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
constexpr std::string_view control_message_name(ControlMessageType type) {
    switch (type) {
    case ControlMessageType::AUTH_IDENTITY:             return "AUTH_IDENTITY";
    case ControlMessageType::CHANNEL_JOIN:              return "CHANNEL_JOIN";
    case ControlMessageType::CHANNEL_LEAVE:             return "CHANNEL_LEAVE";
    case ControlMessageType::KEEPALIVE_PING:            return "KEEPALIVE_PING";
    case ControlMessageType::VOICE_STATE_UPDATE:        return "VOICE_STATE_UPDATE";
    case ControlMessageType::SCREEN_SHARE_START:        return "SCREEN_SHARE_START";
    case ControlMessageType::SCREEN_SHARE_STOP:         return "SCREEN_SHARE_STOP";
    case ControlMessageType::SCREEN_SHARE_VIEW:         return "SCREEN_SHARE_VIEW";
    case ControlMessageType::SCREEN_SHARE_UPDATE:       return "SCREEN_SHARE_UPDATE";
    case ControlMessageType::AUTH_RESPONSE:             return "AUTH_RESPONSE";
    case ControlMessageType::CHANNEL_LIST:              return "CHANNEL_LIST";
    case ControlMessageType::CHANNEL_USER_LIST:         return "CHANNEL_USER_LIST";
    case ControlMessageType::USER_JOINED_CHANNEL:       return "USER_JOINED_CHANNEL";
    case ControlMessageType::USER_LEFT_CHANNEL:         return "USER_LEFT_CHANNEL";
    case ControlMessageType::USER_VOICE_STATE:          return "USER_VOICE_STATE";
    case ControlMessageType::USER_ROLE_CHANGED:         return "USER_ROLE_CHANGED";
    case ControlMessageType::KEEPALIVE_PONG:            return "KEEPALIVE_PONG";
    case ControlMessageType::RETIRED_CHANNEL_KEY:       return "RETIRED_CHANNEL_KEY";
    case ControlMessageType::SCREEN_SHARE_STARTED:      return "SCREEN_SHARE_STARTED";
    case ControlMessageType::SCREEN_SHARE_STOPPED:      return "SCREEN_SHARE_STOPPED";
    case ControlMessageType::SCREEN_SHARE_DENIED:       return "SCREEN_SHARE_DENIED";
    case ControlMessageType::SCREEN_SHARE_VIEWER:       return "SCREEN_SHARE_VIEWER";
    case ControlMessageType::SERVER_ERROR:              return "SERVER_ERROR";
    case ControlMessageType::ADMIN_CREATE_CHANNEL:      return "ADMIN_CREATE_CHANNEL";
    case ControlMessageType::ADMIN_DELETE_CHANNEL:      return "ADMIN_DELETE_CHANNEL";
    case ControlMessageType::ADMIN_SET_ROLE:            return "ADMIN_SET_ROLE";
    case ControlMessageType::ADMIN_KICK_USER:           return "ADMIN_KICK_USER";
    case ControlMessageType::ADMIN_RENAME_CHANNEL:      return "ADMIN_RENAME_CHANNEL";
    case ControlMessageType::ADMIN_RESULT:              return "ADMIN_RESULT";
    case ControlMessageType::CHAT_SEND:                 return "CHAT_SEND";
    case ControlMessageType::CHAT_HISTORY_REQ:          return "CHAT_HISTORY_REQ";
    case ControlMessageType::CHAT_PIN:                  return "CHAT_PIN";
    case ControlMessageType::CHAT_UNPIN:                return "CHAT_UNPIN";
    case ControlMessageType::CHAT_DELETE:               return "CHAT_DELETE";
    case ControlMessageType::CHAT_FILE_UPLOAD_REQ:      return "CHAT_FILE_UPLOAD_REQ";
    case ControlMessageType::CHAT_FILE_DOWNLOAD_REQ:    return "CHAT_FILE_DOWNLOAD_REQ";
    case ControlMessageType::CHAT_SEARCH:               return "CHAT_SEARCH";
    case ControlMessageType::CHAT_PINNED_REQ:           return "CHAT_PINNED_REQ";
    case ControlMessageType::CHAT_MESSAGE:              return "CHAT_MESSAGE";
    case ControlMessageType::CHAT_HISTORY_RESP:         return "CHAT_HISTORY_RESP";
    case ControlMessageType::CHAT_MESSAGE_DELETED:      return "CHAT_MESSAGE_DELETED";
    case ControlMessageType::CHAT_FILE_UPLOAD_RESP:     return "CHAT_FILE_UPLOAD_RESP";
    case ControlMessageType::CHAT_FILE_READY:           return "CHAT_FILE_READY";
    case ControlMessageType::CHAT_SEARCH_RESP:          return "CHAT_SEARCH_RESP";
    case ControlMessageType::CHAT_PINNED_RESP:          return "CHAT_PINNED_RESP";
    case ControlMessageType::CHAT_CHANNEL_LIST:         return "CHAT_CHANNEL_LIST";
    case ControlMessageType::ADMIN_CREATE_TEXT_CHANNEL: return "ADMIN_CREATE_TEXT_CHANNEL";
    case ControlMessageType::ADMIN_DELETE_TEXT_CHANNEL: return "ADMIN_DELETE_TEXT_CHANNEL";
    }
    return "UNKNOWN";
}
#pragma GCC diagnostic pop

// Data plane packet types (first byte of every datagram/stream)
constexpr uint8_t VOICE_PACKET_TYPE        = 0x01;
constexpr uint8_t VIDEO_FRAME_PACKET_TYPE  = 0x02;
constexpr uint8_t VIDEO_CONTROL_TYPE       = 0x03;
constexpr uint8_t STREAM_AUDIO_PACKET_TYPE = 0x04;  // Screen share audio (Opus, stereo)
constexpr uint8_t VOICE2_PACKET_TYPE       = 0x05;  // Secondary per-user voice stream (Opus, mono).
                                                    // Optional companion to VOICE: same wire shape,
                                                    // forwarded identically by the SFU. Carries
                                                    // auxiliary audio (karaoke backing track, channel
                                                    // join sound, plugin audio) that the receiver
                                                    // mixes WITHOUT voice makeup/normalization and at
                                                    // its own volume. Peers that don't know it ignore
                                                    // the type, so it is fully back-compatible.

// File transfer stream type bytes (first byte on client-opened bidirectional streams 2+)
constexpr uint8_t STREAM_TYPE_FILE_UPLOAD   = 0x10;
constexpr uint8_t STREAM_TYPE_FILE_DOWNLOAD = 0x11;

// Per-frame video stream (1.2+). Every encoded screen-share frame travels on
// its own UNIDIRECTIONAL QUIC stream so a lost packet only delays that frame:
//   sharer -> server : [0x12][frame header(14)][encoded]           (one StreamSend + FIN)
//   server -> viewer : [0x12][sender_id(4)][frame header(14)][encoded]
// The 14-byte frame header is the same layout as on the legacy video stream 1
// (see parties/video_common.h, VideoFrameHeader). Frames larger than
// VIDEO_FRAME_MAX_BYTES are rejected and the stream aborted.
constexpr uint8_t STREAM_TYPE_VIDEO_FRAME   = 0x12;

// Video control subtypes (VIDEO_CONTROL_TYPE datagrams / stream-1 packets)
enum class VideoControl : uint8_t {
    Pli                    = 0x01,
    ReservedShareStart     = 0x02,
    ReservedShareStop      = 0x03,
    ReservedNack           = 0x04,
    ReservedReceiverReport = 0x05,
};

#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
constexpr std::string_view video_control_name(VideoControl control) {
    switch (control) {
    case VideoControl::Pli:                    return "Pli";
    case VideoControl::ReservedShareStart:     return "ReservedShareStart";
    case VideoControl::ReservedShareStop:      return "ReservedShareStop";
    case VideoControl::ReservedNack:           return "ReservedNack";
    case VideoControl::ReservedReceiverReport: return "ReservedReceiverReport";
    }
    return "UNKNOWN";
}
#pragma GCC diagnostic pop

// CHAT_HISTORY_REQ / CHAT_SEARCH before_id: no upper bound, start from the newest.
constexpr uint64_t CHAT_BEFORE_ID_LATEST = 0;
constexpr uint16_t CHAT_PAGE_SIZE        = 30;

using ChannelSortOrder     = uint32_t;
using AttachmentMime       = std::string;
using AdminResultStatus    = uint8_t;
using ReservedSessionToken = std::array<uint8_t, 32>;
using AnnouncedShareCodec  = uint8_t;
using AnnouncedShareWidth  = uint16_t;
using AnnouncedShareHeight = uint16_t;

// Control-message wire notes for optional trailing fields (1.2+). Readers must
// tolerate their absence (older peer) and ignore unknown trailing bytes.
//   AUTH_RESPONSE: [user_id(4)][session_token(32)][role(1)][server_name(string)][protocol_version(2)]
//   The trailing u16 tells the client which protocol the server speaks; absent
//   means PROTOCOL_VERSION_ASSUMED_LEGACY.

}
