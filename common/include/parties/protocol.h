#pragma once

#include <cstdint>

namespace parties::protocol {

constexpr uint16_t DEFAULT_PORT = 7800;

// Protocol wire version. Increment MINOR for backwards-compatible additions
// (new optional message types/fields), MAJOR for breaking changes. The two are
// packed into one u16 (major in the high byte, minor in the low byte) and sent
// in AUTH_IDENTITY. The server rejects only on a MAJOR mismatch, so a minor
// bump doesn't lock out older clients.
constexpr uint8_t  PROTOCOL_VERSION_MAJOR = 1;
// 1.1: +VOICE2 (secondary voice stream), additive.
// 1.2: +per-frame unidirectional video streams (STREAM_TYPE_VIDEO_FRAME),
//      SCREEN_SHARE_VIEWER, trailing protocol version in AUTH_RESPONSE,
//      trailing replay flag in SCREEN_SHARE_STARTED. All additive.
// 1.3: +trailing voice state [muted][deafened] in CHANNEL_JOIN and
//      USER_JOINED_CHANNEL. Additive.
// 1.4: +camera streams: a user publishes one video stream per VideoSource,
//      identified on the wire by a stream id (see video_stream_id). Trailing
//      [source] in SCREEN_SHARE_START/UPDATE/STOP, per-frame ingress type
//      STREAM_TYPE_VIDEO_SOURCE_FRAME, trailing [stream_id] on server -> sharer
//      PLIs. Screen shares keep stream id == user id. Additive.
constexpr uint8_t  PROTOCOL_VERSION_MINOR = 4;
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

// Version from which the voice state rides on CHANNEL_JOIN (client -> server)
// and USER_JOINED_CHANNEL (server -> client). A client talking to an older
// server follows the join with a VOICE_STATE_UPDATE; the server additionally
// sends older clients a USER_VOICE_STATE for a joiner that is muted/deafened.
constexpr uint16_t PROTOCOL_VERSION_JOIN_VOICE_STATE = (1u << 8) | 3u;
constexpr bool protocol_supports_join_voice_state(uint16_t version) {
    return protocol_major(version) == 1 && version >= PROTOCOL_VERSION_JOIN_VOICE_STATE;
}

// ── Video sources and stream ids (1.4+) ─────────────────────────────────────
// A user publishes at most one video stream per source. Everything that names
// a stream on the wire (SCREEN_SHARE_STARTED/STOPPED/VIEW, the sender field of
// forwarded frames, PLI targets) carries a 32-bit stream id that packs the
// owner and the source: user_id | (source << 30). A screen share therefore
// keeps stream id == user id, exactly what pre-1.4 peers have always seen;
// camera streams are only ever announced to 1.4+ peers.
enum class VideoSource : uint8_t {
    Screen = 0,
    Camera = 1,
};
constexpr uint8_t  VIDEO_SOURCE_COUNT        = 2;
constexpr uint32_t VIDEO_STREAM_SOURCE_SHIFT = 30;
constexpr uint32_t VIDEO_STREAM_USER_MASK    = (1u << VIDEO_STREAM_SOURCE_SHIFT) - 1;

constexpr bool video_source_valid(uint8_t source) { return source < VIDEO_SOURCE_COUNT; }
constexpr uint32_t video_stream_id(uint32_t user_id, VideoSource source) {
    return (user_id & VIDEO_STREAM_USER_MASK) |
           (static_cast<uint32_t>(source) << VIDEO_STREAM_SOURCE_SHIFT);
}
constexpr uint32_t video_stream_owner(uint32_t stream_id) {
    return stream_id & VIDEO_STREAM_USER_MASK;
}
constexpr VideoSource video_stream_source(uint32_t stream_id) {
    return static_cast<VideoSource>(stream_id >> VIDEO_STREAM_SOURCE_SHIFT);
}
// False for ids no source maps to (source bits beyond VIDEO_SOURCE_COUNT).
constexpr bool video_stream_valid(uint32_t stream_id) {
    return video_stream_owner(stream_id) != 0 &&
           (stream_id >> VIDEO_STREAM_SOURCE_SHIFT) < VIDEO_SOURCE_COUNT;
}

static_assert(video_stream_id(42, VideoSource::Screen) == 42,
              "a screen share's stream id must stay its owner's user id");
static_assert(video_stream_owner(video_stream_id(42, VideoSource::Camera)) == 42 &&
              video_stream_source(video_stream_id(42, VideoSource::Camera)) == VideoSource::Camera,
              "stream ids must round-trip owner and source");

// Version a peer must report before camera streams are announced to it (as a
// viewer) or accepted from it (as a publisher).
constexpr uint16_t PROTOCOL_VERSION_CAMERA = (1u << 8) | 4u;
constexpr bool protocol_supports_camera(uint16_t version) {
    return protocol_major(version) == 1 && version >= PROTOCOL_VERSION_CAMERA;
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
    // 0x0109 was CHANNEL_KEY — removed (was dead crypto; media is protected by
    // QUIC TLS hop-by-hop, the SFU is trusted). Do not reuse without a bump.
    SCREEN_SHARE_STARTED  = 0x010A,   // [sharer_id(4)][codec(1)][width(2)][height(2)][replay(1), optional since 1.2:
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
// Per-frame stream from a publisher, any VideoSource (1.4+; used for every
// source except the screen, which keeps 0x12 for pre-1.4 servers):
//   sharer -> server : [0x13][source(1)][frame header(14)][encoded]   (one StreamSend + FIN)
// The server forwards it to viewers as a 0x12 stream whose sender field is the
// stream id, so receivers need no second format.
constexpr uint8_t STREAM_TYPE_VIDEO_SOURCE_FRAME = 0x13;

// Video control subtypes (VIDEO_CONTROL_TYPE datagrams / stream-1 packets)
constexpr uint8_t VIDEO_CTL_PLI         = 0x01;
constexpr uint8_t VIDEO_CTL_SHARE_START = 0x02;   // reserved, unused
constexpr uint8_t VIDEO_CTL_SHARE_STOP  = 0x03;   // reserved, unused
// 0x04 (NACK) and 0x05 (RECEIVER_REPORT) are reserved for a possible
// datagram-based video path; not implemented. Do not reuse.

// Control-message wire notes for optional trailing fields (1.2+). Readers must
// tolerate their absence (older peer) and ignore unknown trailing bytes.
//   AUTH_RESPONSE: [user_id(4)][session_token(32)][role(1)][server_name(string)][protocol_version(2)]
//   The trailing u16 tells the client which protocol the server speaks; absent
//   means PROTOCOL_VERSION_ASSUMED_LEGACY.
//   CHANNEL_JOIN (1.3+): [channel_id(4)][muted(1)][deafened(1)]
//   The joiner's current voice state, applied atomically with the join so the
//   channel never sees a muted user as live. Absent = keep the session's state.
//   USER_JOINED_CHANNEL (1.3+): [user_id(4)][name(string)][channel_id(4)][role(1)][muted(1)][deafened(1)]
//   Absent = not muted, not deafened.
//   SCREEN_SHARE_START / SCREEN_SHARE_UPDATE (1.4+): [codec(1)][width(2)][height(2)][source(1)]
//   SCREEN_SHARE_STOP (1.4+): [source(1)]
//   The VideoSource the message is about; absent = VideoSource::Screen.
//   SCREEN_SHARE_STARTED / STOPPED / VIEW: the sharer/target field is a stream
//   id (1.4+; identical to the user id for screen shares).
//   PLI toward a sharer (1.4+): [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][requester(4)][stream_id(4)]
//   The stream that needs a keyframe; absent = the sharer's screen share.

} // namespace parties::protocol
