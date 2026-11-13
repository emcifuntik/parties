//! Stream types (`docs/rust-rewrite/03-protocol-v2.md` section 2).
//!
//! Every stream starts with its type as a QUIC variable-length integer. The
//! role of a stream never depends on the order in which streams are opened.
//!
//! A peer that receives a type it does not know resets the stream with
//! [`StreamResetCode::UnknownStreamType`] and keeps the connection: a newer
//! peer may open kinds of streams an older one has never heard of.

use bytes::BufMut;
use thiserror::Error;

use crate::codes::StreamResetCode;
use crate::varint::VarInt;

/// What a stream carries.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StreamType {
    /// `0x00`: the control stream. Requests, responses and events, as
    /// frames. The first stream of a connection, opened by the client, and
    /// the only one allowed before authentication.
    Control,
    /// `0x01`: one bulk request (history, search, member pages, audit log)
    /// and its framed result chunks. Opened by the client.
    Bulk,
    /// `0x02`: one file upload. Opened by the client.
    Upload,
    /// `0x03`: one file download. Opened by the client.
    Download,
    /// `0x04`: one profile picture upload
    /// (`docs/rust-rewrite/13-profiles-access-and-hardware-keys.md`
    /// section 3.4). Opened by the client.
    PictureUpload,
    /// `0x10`: one video frame. Opened by the publisher toward the server
    /// and by the server toward each viewer.
    VideoFrame,
}

/// Whether both ends write on a stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StreamDirection {
    /// Both ends write.
    Bidirectional,
    /// Only the end that opened the stream writes.
    Unidirectional,
}

/// A stream starts with a type this protocol version does not know. The
/// receiver resets the stream and keeps the connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("unknown stream type {0:#x}")]
pub struct UnknownStreamType(pub u64);

impl UnknownStreamType {
    /// The code to reset the stream with:
    /// [`StreamResetCode::UnknownStreamType`].
    #[must_use]
    pub const fn reset_code(self) -> StreamResetCode {
        StreamResetCode::UnknownStreamType
    }
}

impl StreamType {
    /// Every stream type, in ascending order of its number.
    pub const ALL: [Self; 6] = [
        Self::Control,
        Self::Bulk,
        Self::Upload,
        Self::Download,
        Self::PictureUpload,
        Self::VideoFrame,
    ];

    /// The number the stream starts with.
    #[must_use]
    pub const fn code(self) -> u32 {
        match self {
            Self::Control => 0x00,
            Self::Bulk => 0x01,
            Self::Upload => 0x02,
            Self::Download => 0x03,
            Self::PictureUpload => 0x04,
            Self::VideoFrame => 0x10,
        }
    }

    /// Reads the number a stream started with.
    pub const fn from_code(code: u64) -> Result<Self, UnknownStreamType> {
        Ok(match code {
            0x00 => Self::Control,
            0x01 => Self::Bulk,
            0x02 => Self::Upload,
            0x03 => Self::Download,
            0x04 => Self::PictureUpload,
            0x10 => Self::VideoFrame,
            other => return Err(UnknownStreamType(other)),
        })
    }

    /// Whether streams of this type are bidirectional.
    #[must_use]
    pub const fn direction(self) -> StreamDirection {
        match self {
            Self::Control | Self::Bulk | Self::Upload | Self::Download | Self::PictureUpload => {
                StreamDirection::Bidirectional
            }
            Self::VideoFrame => StreamDirection::Unidirectional,
        }
    }

    /// Whether a connection that has not authenticated yet may open a
    /// stream of this type. Only the control stream.
    #[must_use]
    pub const fn allowed_before_authentication(self) -> bool {
        matches!(self, Self::Control)
    }

    /// Appends the type to `dst`, as the first bytes of a new stream.
    pub fn encode<B: BufMut>(self, dst: &mut B) {
        VarInt::from_u32(self.code()).encode(dst);
    }

    /// Reads the type from the first bytes of a stream.
    ///
    /// Returns the type and the number of bytes it took, or `Ok(None)` when
    /// `src` does not hold the whole integer yet.
    pub fn decode(src: &[u8]) -> Result<Option<(Self, usize)>, UnknownStreamType> {
        match VarInt::decode(src) {
            Ok((code, len)) => Ok(Some((Self::from_code(code.get())?, len))),
            Err(_) => Ok(None),
        }
    }
}

impl TryFrom<VarInt> for StreamType {
    type Error = UnknownStreamType;

    fn try_from(code: VarInt) -> Result<Self, Self::Error> {
        Self::from_code(code.get())
    }
}

impl From<StreamType> for VarInt {
    fn from(stream_type: StreamType) -> Self {
        Self::from_u32(stream_type.code())
    }
}
