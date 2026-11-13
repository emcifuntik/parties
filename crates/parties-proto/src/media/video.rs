//! Video frame streams (`docs/rust-rewrite/03-protocol-v2.md` section 6.3).
//!
//! One unidirectional stream carries one frame:
//!
//! ```text
//! +-----------+-----------------+----------------------+-----------+
//! | type 0x10 | track_id varint | frame header (16 B)  |  payload  |
//! +-----------+-----------------+----------------------+-----------+   then FIN
//! ```
//!
//! ```text
//! frame header, little-endian
//!   u32 frame_seq        stamped after encode, wrap-safe comparison
//!   u32 capture_ts_ms    sender's monotonic capture time
//!   u8  flags            bit0 keyframe, bit1 discardable, bit2 encrypted (reserved)
//!   u8  layer            high nibble spatial, low nibble temporal; 0 today
//!   u8  codec            1 AV1, 2 H.265, 3 H.264
//!   u8  reserved         0
//!   u16 width
//!   u16 height
//! ```
//!
//! A whole stream may not exceed
//! [`MAX_VIDEO_FRAME_STREAM_LEN`](crate::limits::MAX_VIDEO_FRAME_STREAM_LEN).
//! The server forwards a frame to each viewer on a new stream with identical
//! bytes, so the payload is never copied or parsed here: the decoder returns
//! the header and where the payload starts.
//!
//! # Example
//!
//! ```
//! use parties_proto::ids::TrackId;
//! use parties_proto::media::{
//!     VideoCodecId, VideoFlags, VideoFrameHeader, VideoLayer, VideoStreamHeader,
//! };
//!
//! let header = VideoStreamHeader {
//!     track: TrackId(9),
//!     frame: VideoFrameHeader {
//!         frame_seq: 1,
//!         capture_ts_ms: 33,
//!         flags: VideoFlags::KEYFRAME,
//!         layer: VideoLayer::BASE,
//!         codec: VideoCodecId::AV1,
//!         width: 1920,
//!         height: 1080,
//!     },
//! };
//! let mut stream = Vec::new();
//! header.encode(&mut stream);
//! stream.extend_from_slice(b"encoded frame");
//!
//! // The receiver feeds what it has read so far, from the first byte of
//! // the stream, until the header is complete.
//! assert_eq!(VideoStreamHeader::decode(&stream[..10])?, None);
//! let (decoded, payload_at) = VideoStreamHeader::decode(&stream)?.expect("complete");
//! assert_eq!(decoded, header);
//! assert_eq!(&stream[payload_at..], b"encoded frame");
//! # Ok::<(), parties_proto::media::VideoStreamError>(())
//! ```

use bytes::BufMut;
use thiserror::Error;

use super::flags_type;
use crate::codes::StreamResetCode;
use crate::ids::TrackId;
use crate::stream::StreamType;
use crate::v2;
use crate::varint::VarInt;

flags_type! {
    /// The `flags` byte of a video frame header.
    VideoFlags {
        /// Bit 0: the frame is a keyframe. It is sent at a higher stream
        /// priority than delta frames.
        KEYFRAME = 0;
        /// Bit 1: no other frame depends on this one. A placeholder for
        /// temporal scalability; not set in 2.0.
        DISCARDABLE = 1;
        /// Bit 2: the payload is end-to-end encrypted. Reserved for a later
        /// minor version (section 6.5).
        ENCRYPTED = 2;
    }
}

/// The `layer` byte of a video frame header: the spatial layer in the high
/// nibble, the temporal layer in the low nibble. A placeholder for
/// simulcast and temporal scalability; 2.0 sends single-layer video, layer
/// 0.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct VideoLayer(pub u8);

impl VideoLayer {
    /// Spatial layer 0, temporal layer 0: what 2.0 sends.
    pub const BASE: Self = Self(0);

    /// Builds the byte from the two layer indices. Returns `None` when one
    /// does not fit a nibble.
    #[must_use]
    pub const fn new(spatial: u8, temporal: u8) -> Option<Self> {
        if spatial < 16 && temporal < 16 {
            Some(Self((spatial << 4) | temporal))
        } else {
            None
        }
    }

    /// The spatial layer: the high nibble.
    #[must_use]
    pub const fn spatial(self) -> u8 {
        self.0 >> 4
    }

    /// The temporal layer: the low nibble.
    #[must_use]
    pub const fn temporal(self) -> u8 {
        self.0 & 0x0F
    }
}

/// The `codec` byte of a video frame header. The numbers are those of
/// [`v2::VideoCodec`].
///
/// The byte is kept as it was received, so that a header with a codec this
/// version does not know still decodes: the server forwards the frame, and
/// only a viewer needs to understand it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct VideoCodecId(pub u8);

impl VideoCodecId {
    /// 1: AV1.
    pub const AV1: Self = Self(1);
    /// 2: H.265.
    pub const H265: Self = Self(2);
    /// 3: H.264.
    pub const H264: Self = Self(3);

    /// The codec as the control plane names it, or `None` for a number this
    /// version does not know (0 included).
    #[must_use]
    pub fn known(self) -> Option<v2::VideoCodec> {
        match v2::VideoCodec::try_from(i32::from(self.0)) {
            Ok(v2::VideoCodec::Unspecified) | Err(_) => None,
            Ok(codec) => Some(codec),
        }
    }
}

impl From<v2::VideoCodec> for VideoCodecId {
    fn from(codec: v2::VideoCodec) -> Self {
        match codec {
            v2::VideoCodec::Unspecified => Self(0),
            v2::VideoCodec::Av1 => Self::AV1,
            v2::VideoCodec::H265 => Self::H265,
            v2::VideoCodec::H264 => Self::H264,
        }
    }
}

/// The 16-byte frame header of a video frame stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct VideoFrameHeader {
    /// The sequence number of the frame within its track, stamped after
    /// encoding. It wraps; compare it wrap-safely.
    pub frame_seq: u32,
    /// The sender's monotonic capture time in milliseconds; it wraps.
    pub capture_ts_ms: u32,
    /// The flags.
    pub flags: VideoFlags,
    /// The layer; [`VideoLayer::BASE`] in 2.0.
    pub layer: VideoLayer,
    /// The codec of the payload.
    pub codec: VideoCodecId,
    /// The width of the encoded frame in pixels.
    pub width: u16,
    /// The height of the encoded frame in pixels.
    pub height: u16,
}

impl VideoFrameHeader {
    /// The length of the frame header on the wire.
    pub const LEN: usize = 16;

    /// Whether the frame is a keyframe.
    #[must_use]
    pub const fn is_keyframe(&self) -> bool {
        self.flags.contains(VideoFlags::KEYFRAME)
    }

    /// The 16 bytes of the header. The reserved byte is zero.
    #[must_use]
    pub const fn to_bytes(&self) -> [u8; Self::LEN] {
        let seq = self.frame_seq.to_le_bytes();
        let ts = self.capture_ts_ms.to_le_bytes();
        let width = self.width.to_le_bytes();
        let height = self.height.to_le_bytes();
        [
            seq[0],
            seq[1],
            seq[2],
            seq[3],
            ts[0],
            ts[1],
            ts[2],
            ts[3],
            self.flags.bits(),
            self.layer.0,
            self.codec.0,
            0,
            width[0],
            width[1],
            height[0],
            height[1],
        ]
    }

    /// Reads the header from its 16 bytes. Every input is a header: the
    /// reserved byte and reserved flag bits are ignored.
    #[must_use]
    pub const fn from_bytes(bytes: &[u8; Self::LEN]) -> Self {
        let [
            s0,
            s1,
            s2,
            s3,
            t0,
            t1,
            t2,
            t3,
            flags,
            layer,
            codec,
            _reserved,
            w0,
            w1,
            h0,
            h1,
        ] = *bytes;
        Self {
            frame_seq: u32::from_le_bytes([s0, s1, s2, s3]),
            capture_ts_ms: u32::from_le_bytes([t0, t1, t2, t3]),
            flags: VideoFlags::from_bits_truncate(flags),
            layer: VideoLayer(layer),
            codec: VideoCodecId(codec),
            width: u16::from_le_bytes([w0, w1]),
            height: u16::from_le_bytes([h0, h1]),
        }
    }
}

/// Why the start of a stream is not the header of a video frame stream.
/// The receiver resets the stream; the connection stays open.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum VideoStreamError {
    /// The stream does not start with the type of a video frame stream.
    #[error("stream type {0:#x} is not a video frame stream")]
    NotAVideoStream(u64),
    /// The track id is not a 32-bit value.
    #[error("the track id {0} does not fit 32 bits")]
    TrackIdOutOfRange(u64),
}

impl VideoStreamError {
    /// The code to reset the stream with: an unknown type is
    /// `UNKNOWN_STREAM_TYPE`, a known type that is not a video frame stream
    /// is `NOT_ALLOWED` where a video frame stream is expected, and a track
    /// id out of range is `MALFORMED`.
    #[must_use]
    pub const fn reset_code(&self) -> StreamResetCode {
        match self {
            Self::NotAVideoStream(code) => match StreamType::from_code(*code) {
                Ok(_) => StreamResetCode::NotAllowed,
                Err(_) => StreamResetCode::UnknownStreamType,
            },
            Self::TrackIdOutOfRange(_) => StreamResetCode::Malformed,
        }
    }
}

/// Everything a video frame stream carries before the payload: the stream
/// type, the track id and the frame header.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct VideoStreamHeader {
    /// The track the frame belongs to.
    pub track: TrackId,
    /// The frame header.
    pub frame: VideoFrameHeader,
}

impl VideoStreamHeader {
    /// The length of the longest header this crate writes: the type, an
    /// 8-byte track id and the frame header.
    pub const MAX_LEN: usize = 1 + VarInt::MAX_LEN + VideoFrameHeader::LEN;

    /// The length of this header on the wire, stream type included.
    #[must_use]
    pub const fn encoded_len(&self) -> usize {
        VarInt::from_u32(StreamType::VideoFrame.code()).encoded_len()
            + VarInt::from_u32(self.track.0).encoded_len()
            + VideoFrameHeader::LEN
    }

    /// Appends the header to `dst`, stream type first. The encoded frame
    /// goes right behind it.
    ///
    /// Panics, like every [`BufMut`] writer, when a fixed-size buffer has
    /// fewer than [`VideoStreamHeader::encoded_len`] bytes left.
    pub fn encode<B: BufMut>(&self, dst: &mut B) {
        StreamType::VideoFrame.encode(dst);
        VarInt::from_u32(self.track.0).encode(dst);
        dst.put_slice(&self.frame.to_bytes());
    }

    /// Reads the header from the first bytes of a stream.
    ///
    /// Returns the header and the offset at which the payload starts, or
    /// `Ok(None)` when `stream` does not hold the whole header yet. A
    /// stream that finishes before its header is complete is malformed.
    /// Never panics.
    pub fn decode(stream: &[u8]) -> Result<Option<(Self, usize)>, VideoStreamError> {
        let Ok((stream_type, type_len)) = VarInt::decode(stream) else {
            return Ok(None);
        };
        if stream_type.get() != u64::from(StreamType::VideoFrame.code()) {
            return Err(VideoStreamError::NotAVideoStream(stream_type.get()));
        }
        let rest = &stream[type_len..];
        Ok(Self::decode_after_type(rest)?.map(|(header, len)| (header, type_len + len)))
    }

    /// Reads the header from the bytes that follow the stream type, for a
    /// receiver that has already read the type to route the stream.
    ///
    /// Returns the header and the offset in `rest` at which the payload
    /// starts, or `Ok(None)` when `rest` does not hold the whole header yet.
    /// Never panics.
    pub fn decode_after_type(rest: &[u8]) -> Result<Option<(Self, usize)>, VideoStreamError> {
        let Ok((raw_track, track_len)) = VarInt::decode(rest) else {
            return Ok(None);
        };
        let track = u32::try_from(raw_track.get())
            .map_err(|_| VideoStreamError::TrackIdOutOfRange(raw_track.get()))?;
        let Some((frame, _)) = rest[track_len..].split_first_chunk::<{ VideoFrameHeader::LEN }>()
        else {
            return Ok(None);
        };
        let header = Self {
            track: TrackId(track),
            frame: VideoFrameHeader::from_bytes(frame),
        };
        Ok(Some((header, track_len + VideoFrameHeader::LEN)))
    }
}
