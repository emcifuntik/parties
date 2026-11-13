//! Every bound of the protocol as a named constant
//! (`docs/rust-rewrite/02-target-architecture.md` section 2, principle 7:
//! every queue, buffer, stream count and message size has a named bound
//! here).
//!
//! "03" below is `docs/rust-rewrite/03-protocol-v2.md`, "04" is
//! `docs/rust-rewrite/04-connection-security.md` and "13" is
//! `docs/rust-rewrite/13-profiles-access-and-hardware-keys.md`.
//!
//! The rules behind the numbers (04 section 8): nothing is buffered without
//! a cap that is checked while receiving, and the memory a connection can
//! claim before it has authenticated is a small constant.

use core::time::Duration;

const KIB: usize = 1024;
const MIB: u64 = 1024 * 1024;

// ---------------------------------------------------------------------------
// Transport profile (03 section 1)
// ---------------------------------------------------------------------------

/// The QUIC idle timeout (03 section 1).
pub const IDLE_TIMEOUT: Duration = Duration::from_secs(30);

/// How often the client sends a QUIC keep-alive (03 section 1).
pub const KEEP_ALIVE_INTERVAL: Duration = Duration::from_secs(10);

// ---------------------------------------------------------------------------
// Before authentication (03 sections 4 and 9; 04 section 6.1)
// ---------------------------------------------------------------------------

/// The time from accepting a connection to a successful `AuthResult`,
/// independent of keep-alives (03 sections 4 and 9; 04 section 6.1,
/// "Deadline").
pub const AUTH_DEADLINE: Duration = Duration::from_secs(10);

/// How many `Authenticate` attempts one connection gets (03 section 4.2 and
/// section 9, rate class "Authentication").
pub const MAX_AUTH_ATTEMPTS: u32 = 3;

/// The longest control frame before authentication, in bytes of the message
/// (03 section 9; 04 section 6.1, "Budget"). It applies in both directions
/// until the handshake ends: to `ClientHandshake` frames, and to
/// `ServerHandshake` frames other than `Ready`.
pub const MAX_CONTROL_FRAME_LEN_PRE_AUTH: usize = 4 * KIB;

/// How many bidirectional streams a connection may open before it has
/// authenticated: the control stream (03 sections 4 and 9; 04 section 6.1,
/// "Budget").
pub const MAX_BIDI_STREAMS_PRE_AUTH: u64 = 1;

/// How many unidirectional streams a connection may open before it has
/// authenticated: none. Its datagrams are dropped unread (03 sections 4
/// and 9).
pub const MAX_UNI_STREAMS_PRE_AUTH: u64 = 0;

// ---------------------------------------------------------------------------
// After authentication (03 sections 7 and 9)
// ---------------------------------------------------------------------------

/// The longest control frame after authentication, in bytes of the message
/// (03 section 9). `Ready` frames are bound by it too: a larger snapshot is
/// sent as several frames (03 section 5.3).
pub const MAX_CONTROL_FRAME_LEN: usize = 64 * KIB;

/// The longest frame on a bulk stream, in bytes of the message: the request
/// and each result chunk (03 sections 7 and 9).
pub const MAX_BULK_FRAME_LEN: usize = 64 * KIB;

/// How many bidirectional streams a connection may have open after
/// authentication: 1 control, 4 bulk, 4 file, and spare (03 section 9).
pub const MAX_BIDI_STREAMS: u64 = 16;

/// How many unidirectional streams a connection may have open after
/// authentication: video frames (03 section 9).
pub const MAX_UNI_STREAMS: u64 = 64;

/// How many bulk request streams may be open per connection (03 sections 7
/// and 9).
pub const MAX_BULK_STREAMS: u64 = 4;

/// How many file transfer streams may be open per connection: uploads,
/// downloads and picture uploads together (03 section 9).
pub const MAX_FILE_STREAMS: u64 = 4;

/// The receive window of a unidirectional stream, in bytes (03 section 9).
pub const UNI_STREAM_RECEIVE_WINDOW: u64 = 4 * MIB;

/// How many `Ready` frames one state snapshot may have (03 sections 5.3
/// and 9). With [`MAX_CONTROL_FRAME_LEN`] that bounds a snapshot to 1 MiB on
/// the wire, several times what the directory of 1,000 members needs.
pub const MAX_READY_FRAMES: usize = 16;

/// How many entries the repeated fields of one state snapshot may hold
/// together: roles, channels, per-channel permissions, voice states, tracks
/// and members (03 sections 5.3 and 9). It bounds what a snapshot costs a
/// client in memory, where an entry is much larger than its shortest
/// encoding.
pub const MAX_READY_ITEMS: usize = 16 * KIB;

/// How many bytes of control frames, length prefixes included, may wait in
/// a connection's outbox. A client that lets it overflow is closed with
/// `SLOW_CONSUMER` (03 section 9). The bound is twice the largest state
/// snapshot: the snapshot fits, with the events that queue up behind it
/// while the client reads it.
pub const MAX_OUTBOX_BYTES: usize = 2 * MAX_READY_FRAMES * MAX_CONTROL_FRAME_LEN;

// ---------------------------------------------------------------------------
// Media plane (03 section 6)
// ---------------------------------------------------------------------------

/// The longest Opus payload of an audio datagram, in bytes (03 sections 6.1
/// and 9).
pub const MAX_AUDIO_PAYLOAD_LEN: usize = 1000;

/// How many audio datagrams per second the server forwards per track;
/// excess is dropped (03 section 6.2, "Rate cap").
pub const MAX_AUDIO_DATAGRAMS_PER_SECOND: u32 = 120;

/// The clock of audio timestamps: samples per second (03 section 6.1).
pub const AUDIO_CLOCK_RATE: u32 = 48_000;

/// The longest video frame stream, in bytes: stream type, track id, frame
/// header and payload together (03 sections 6.3 and 9).
pub const MAX_VIDEO_FRAME_STREAM_LEN: u64 = 4 * MIB;

/// How many bytes of unfinished video frame streams the server buffers per
/// session (03 sections 6.3 and 9).
pub const MAX_BUFFERED_VIDEO_INGRESS: u64 = 16 * MIB;

// ---------------------------------------------------------------------------
// Text (03 section 9; 04 section 7)
// ---------------------------------------------------------------------------

/// The longest chat message in characters, unless the server's settings say
/// otherwise (03 section 9).
pub const DEFAULT_MAX_MESSAGE_CHARS: u32 = 4000;

/// The shortest display name or nickname, in characters after
/// normalisation (03 section 9; 04 section 7).
pub const MIN_DISPLAY_NAME_CHARS: usize = 1;

/// The longest display name or nickname, in characters after normalisation
/// (03 section 9; 04 section 7).
pub const MAX_DISPLAY_NAME_CHARS: usize = 32;

/// The shortest channel or role name, in characters (04 section 7).
pub const MIN_NAME_CHARS: usize = 1;

/// The longest channel or role name, in characters (04 section 7).
pub const MAX_NAME_CHARS: usize = 64;

// ---------------------------------------------------------------------------
// Members, pictures and join codes (13)
// ---------------------------------------------------------------------------

/// Up to this many members, `Ready` carries the whole member directory;
/// above it, only the members who are online (13 section 2).
pub const MAX_MEMBERS_IN_READY: usize = 1000;

/// The length of a picture hash, SHA-512, in bytes (13 section 3.1).
pub const PICTURE_HASH_LEN: usize = 64;

/// The largest profile picture, in bytes (13 sections 3.1 and 3.4).
pub const MAX_PICTURE_LEN: usize = 128 * KIB;

/// The width and the height of a profile picture, in pixels (13
/// section 3.1).
pub const PICTURE_SIDE_PIXELS: u32 = 256;

/// The number of characters of a join code, without the separator it is
/// shown with (13 section 5.2).
pub const JOIN_CODE_LEN: usize = 10;

/// How long a join code is valid unless its creator says otherwise (13
/// section 5.2).
pub const DEFAULT_JOIN_CODE_LIFETIME: Duration = Duration::from_secs(7 * 24 * 60 * 60);

// ---------------------------------------------------------------------------
// Features (03 section 11)
// ---------------------------------------------------------------------------

/// How many feature names `Hello` and `HelloAck` may list (03 section 11).
pub const MAX_FEATURES: usize = 64;

/// The longest feature name, in bytes (03 section 11).
pub const MAX_FEATURE_NAME_LEN: usize = 32;

// ---------------------------------------------------------------------------
// Rate classes (03 section 9)
// ---------------------------------------------------------------------------

/// The budget of a rate class as a token bucket: `count` requests per
/// `period` in the long run, and at most `burst` at once.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RateLimit {
    /// How many requests are allowed per `period`.
    pub count: u32,
    /// The time over which `count` requests are allowed.
    pub period: Duration,
    /// How many requests may be made at once after a quiet time: the size
    /// of the bucket.
    pub burst: u32,
}

impl RateLimit {
    /// `count` requests per second with the given burst.
    #[must_use]
    pub const fn per_second(count: u32, burst: u32) -> Self {
        Self {
            count,
            period: Duration::from_secs(1),
            burst,
        }
    }

    /// The time after which one more request is allowed: `period / count`.
    #[must_use]
    pub const fn interval(&self) -> Duration {
        // `count` is never 0 in a budget of this module; a zero count would
        // mean "never", which the longest duration stands for.
        match self.period.checked_div(self.count) {
            Some(interval) => interval,
            None => Duration::MAX,
        }
    }
}

/// The rate classes of requests (03 section 9, "Rate classes"). A request
/// over budget gets the status `RATE_LIMITED` with `retry_after_ms`;
/// sustained abuse closes the connection.
///
/// Authentication is not a token bucket: see [`MAX_AUTH_ATTEMPTS`]. Chat
/// sends are limited by the channel's slow mode as well.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RateClass {
    /// Presence, typing, voice state.
    Presence,
    /// Join, leave, move.
    Join,
    /// Chat send.
    ChatSend,
    /// History and member pages.
    Pages,
    /// Search.
    Search,
    /// Administrative changes.
    Admin,
    /// Keyframe requests, per session (03 section 6.4).
    KeyframeRequest,
}

impl RateClass {
    /// Every rate class, in the order the design document lists them.
    pub const ALL: [Self; 7] = [
        Self::Presence,
        Self::Join,
        Self::ChatSend,
        Self::Pages,
        Self::Search,
        Self::Admin,
        Self::KeyframeRequest,
    ];

    /// The budget of the class.
    #[must_use]
    pub const fn limit(self) -> RateLimit {
        match self {
            Self::Presence => RATE_PRESENCE,
            Self::Join => RATE_JOIN,
            Self::ChatSend => RATE_CHAT_SEND,
            Self::Pages => RATE_PAGES,
            Self::Search => RATE_SEARCH,
            Self::Admin => RATE_ADMIN,
            Self::KeyframeRequest => RATE_KEYFRAME_REQUEST,
        }
    }
}

/// Presence, typing, voice state: 5 per second, burst 10 (03 section 9).
pub const RATE_PRESENCE: RateLimit = RateLimit::per_second(5, 10);

/// Join, leave, move: 1 per second, burst 5 (03 section 9).
pub const RATE_JOIN: RateLimit = RateLimit::per_second(1, 5);

/// Chat send: 5 per 5 seconds (03 section 9).
pub const RATE_CHAT_SEND: RateLimit = RateLimit {
    count: 5,
    period: Duration::from_secs(5),
    burst: 5,
};

/// History and member pages: 2 per second, burst 4 (03 section 9).
pub const RATE_PAGES: RateLimit = RateLimit::per_second(2, 4);

/// Search: 1 per second, burst 2 (03 section 9).
pub const RATE_SEARCH: RateLimit = RateLimit::per_second(1, 2);

/// Administrative changes: 2 per second, burst 10 (03 section 9).
pub const RATE_ADMIN: RateLimit = RateLimit::per_second(2, 10);

/// Keyframe requests: 4 per second per session, burst 4 (03 sections 6.4
/// and 9).
pub const RATE_KEYFRAME_REQUEST: RateLimit = RateLimit::per_second(4, 4);
