//! Application error codes: why a connection was closed and why a stream
//! was reset (`docs/rust-rewrite/03-protocol-v2.md` section 10).
//!
//! Both kinds are QUIC application error codes and share one number space:
//! close codes are below `0x100`, stream reset codes start there. A code
//! therefore means the same thing wherever it shows up in a log.

use thiserror::Error;

/// Why a connection was closed: the error code of the QUIC
/// `CONNECTION_CLOSE` frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum CloseCode {
    /// A normal close.
    Normal = 0,
    /// The peer broke the protocol: a frame too long or one that does not
    /// decode, a message out of place.
    ProtocolViolation = 1,
    /// Authentication failed. The `AuthResult` sent before says why.
    AuthFailed = 2,
    /// The connection did not authenticate within the deadline.
    AuthTimeout = 3,
    /// A moderator removed the member from the server.
    Kicked = 4,
    /// The member was banned.
    Banned = 5,
    /// The server is shutting down.
    ServerShutdown = 6,
    /// The connection kept exceeding its rate limits.
    RateLimited = 7,
    /// Another session of the same user replaced this one.
    SessionReplaced = 8,
    /// The client did not drain its outbox.
    SlowConsumer = 9,
}

/// What a client does after the server closed the connection with a
/// [`CloseCode`]. The reconnect policy keys off this instead of matching
/// error strings.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ClientReaction {
    /// Nothing: the close was expected.
    None,
    /// Report the error; do not retry by itself.
    Report,
    /// Show the reason of the refusal to the user.
    ShowReason,
    /// Reconnect with exponential backoff.
    RetryWithBackoff,
    /// Reconnect after a delay.
    RetryAfterDelay,
    /// Stay disconnected until the user asks to connect again.
    StayDisconnected,
}

impl ClientReaction {
    /// Whether the client reconnects without being asked.
    #[must_use]
    pub const fn reconnects(self) -> bool {
        matches!(self, Self::RetryWithBackoff | Self::RetryAfterDelay)
    }
}

/// The code is not a close code of this protocol version.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("unknown close code {0}")]
pub struct UnknownCloseCode(pub u64);

impl CloseCode {
    /// Every close code, in ascending order.
    pub const ALL: [Self; 10] = [
        Self::Normal,
        Self::ProtocolViolation,
        Self::AuthFailed,
        Self::AuthTimeout,
        Self::Kicked,
        Self::Banned,
        Self::ServerShutdown,
        Self::RateLimited,
        Self::SessionReplaced,
        Self::SlowConsumer,
    ];

    /// The number on the wire.
    #[must_use]
    pub const fn code(self) -> u32 {
        self as u32
    }

    /// Reads a code received from the peer.
    pub const fn from_code(code: u64) -> Result<Self, UnknownCloseCode> {
        Ok(match code {
            0 => Self::Normal,
            1 => Self::ProtocolViolation,
            2 => Self::AuthFailed,
            3 => Self::AuthTimeout,
            4 => Self::Kicked,
            5 => Self::Banned,
            6 => Self::ServerShutdown,
            7 => Self::RateLimited,
            8 => Self::SessionReplaced,
            9 => Self::SlowConsumer,
            other => return Err(UnknownCloseCode(other)),
        })
    }

    /// The name the design document uses, for example `"SLOW_CONSUMER"`.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Normal => "NORMAL",
            Self::ProtocolViolation => "PROTOCOL_VIOLATION",
            Self::AuthFailed => "AUTH_FAILED",
            Self::AuthTimeout => "AUTH_TIMEOUT",
            Self::Kicked => "KICKED",
            Self::Banned => "BANNED",
            Self::ServerShutdown => "SERVER_SHUTDOWN",
            Self::RateLimited => "RATE_LIMITED",
            Self::SessionReplaced => "SESSION_REPLACED",
            Self::SlowConsumer => "SLOW_CONSUMER",
        }
    }

    /// What the client does when the server closes with this code.
    #[must_use]
    pub const fn client_reaction(self) -> ClientReaction {
        match self {
            Self::Normal => ClientReaction::None,
            Self::ProtocolViolation => ClientReaction::Report,
            Self::AuthFailed => ClientReaction::ShowReason,
            Self::AuthTimeout | Self::ServerShutdown | Self::SlowConsumer => {
                ClientReaction::RetryWithBackoff
            }
            Self::RateLimited => ClientReaction::RetryAfterDelay,
            Self::Kicked | Self::Banned | Self::SessionReplaced => ClientReaction::StayDisconnected,
        }
    }
}

impl TryFrom<u64> for CloseCode {
    type Error = UnknownCloseCode;

    fn try_from(code: u64) -> Result<Self, Self::Error> {
        Self::from_code(code)
    }
}

impl From<CloseCode> for u32 {
    fn from(code: CloseCode) -> Self {
        code.code()
    }
}

/// Why a stream was reset or its sender asked to stop: the error code of
/// the QUIC `RESET_STREAM` and `STOP_SENDING` frames. The connection stays
/// open.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum StreamResetCode {
    /// The stream is no longer needed: a bulk request was abandoned or a
    /// transfer cancelled.
    Cancelled = 0x100,
    /// The stream starts with a type the receiver does not know.
    UnknownStreamType = 0x101,
    /// The session may not open this stream now: it is not authenticated,
    /// it does not publish the track, or it lacks the permission.
    NotAllowed = 0x102,
    /// The stream is longer than its limit.
    TooLarge = 0x103,
    /// Too many streams of this type are open, or the session's buffered
    /// video is over its cap.
    LimitExceeded = 0x104,
    /// The header of the stream does not decode.
    Malformed = 0x105,
    /// The sender dropped a queued video frame that is no longer useful.
    FrameDropped = 0x106,
}

/// The code is not a stream reset code of this protocol version.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("unknown stream reset code {0:#x}")]
pub struct UnknownStreamResetCode(pub u64);

impl StreamResetCode {
    /// Every stream reset code, in ascending order.
    pub const ALL: [Self; 7] = [
        Self::Cancelled,
        Self::UnknownStreamType,
        Self::NotAllowed,
        Self::TooLarge,
        Self::LimitExceeded,
        Self::Malformed,
        Self::FrameDropped,
    ];

    /// The number on the wire.
    #[must_use]
    pub const fn code(self) -> u32 {
        self as u32
    }

    /// Reads a code received from the peer.
    pub const fn from_code(code: u64) -> Result<Self, UnknownStreamResetCode> {
        Ok(match code {
            0x100 => Self::Cancelled,
            0x101 => Self::UnknownStreamType,
            0x102 => Self::NotAllowed,
            0x103 => Self::TooLarge,
            0x104 => Self::LimitExceeded,
            0x105 => Self::Malformed,
            0x106 => Self::FrameDropped,
            other => return Err(UnknownStreamResetCode(other)),
        })
    }

    /// The name the design document uses, for example
    /// `"UNKNOWN_STREAM_TYPE"`.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Cancelled => "CANCELLED",
            Self::UnknownStreamType => "UNKNOWN_STREAM_TYPE",
            Self::NotAllowed => "NOT_ALLOWED",
            Self::TooLarge => "TOO_LARGE",
            Self::LimitExceeded => "LIMIT_EXCEEDED",
            Self::Malformed => "MALFORMED",
            Self::FrameDropped => "FRAME_DROPPED",
        }
    }
}

impl TryFrom<u64> for StreamResetCode {
    type Error = UnknownStreamResetCode;

    fn try_from(code: u64) -> Result<Self, Self::Error> {
        Self::from_code(code)
    }
}

impl From<StreamResetCode> for u32 {
    fn from(code: StreamResetCode) -> Self {
        code.code()
    }
}
