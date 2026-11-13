//! Datagrams (`docs/rust-rewrite/03-protocol-v2.md` section 6.1).
//!
//! ```text
//! +------+-----------------+ ...
//! | type | track_id varint |
//! +------+-----------------+ ...
//! ```
//!
//! | Type | Name | Body after `track_id` |
//! |---|---|---|
//! | `0x01` | Audio | `seq u16`, `timestamp u32` (48 kHz samples), `flags u8`, `level u8`, Opus payload (at most 1000 bytes) |
//! | `0x02` | Keyframe request | `reason u8` |
//! | `0x03`–`0x0F` | Reserved | Feedback and receiver reports |
//!
//! `type` is one byte. Integers are little-endian. A datagram of a type
//! this version does not know is ignored, not an error:
//! [`Datagram::decode`] returns [`Datagram::Unknown`] for it.
//!
//! # Example
//!
//! ```
//! use parties_proto::ids::TrackId;
//! use parties_proto::media::{AudioFlags, AudioHeader, Datagram};
//!
//! let header = AudioHeader {
//!     track: TrackId(5),
//!     seq: 1,
//!     timestamp: 960,
//!     flags: AudioFlags::EMPTY,
//!     level: 40,
//! };
//! let opus = [0xAA, 0xBB];
//!
//! // The sender writes the header, then the Opus frame behind it.
//! let mut datagram = Vec::new();
//! header.encode(&mut datagram);
//! datagram.extend_from_slice(&opus);
//!
//! // The receiver borrows the payload from the datagram.
//! let Datagram::Audio(audio) = Datagram::decode(&datagram)? else {
//!     unreachable!()
//! };
//! assert_eq!(audio.header, header);
//! assert_eq!(audio.payload, &opus);
//! # Ok::<(), parties_proto::media::DatagramError>(())
//! ```

use bytes::BufMut;
use thiserror::Error;

use super::flags_type;
use crate::ids::TrackId;
use crate::limits::MAX_AUDIO_PAYLOAD_LEN;
use crate::varint::VarInt;

/// The type byte of an audio datagram.
pub const TYPE_AUDIO: u8 = 0x01;

/// The type byte of a keyframe request.
pub const TYPE_KEYFRAME_REQUEST: u8 = 0x02;

/// The audio level that stands for silence: 127, in -dBov.
pub const LEVEL_SILENCE: u8 = 127;

/// The length of the fields of an audio datagram between the track id and
/// the payload: `seq`, `timestamp`, `flags`, `level`.
const AUDIO_FIXED_LEN: usize = 8;

/// Why a datagram does not decode. The datagram is dropped; the connection
/// stays open.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum DatagramError {
    /// The datagram has no bytes at all.
    #[error("the datagram is empty")]
    Empty,
    /// The datagram ends inside its header.
    #[error("the datagram ends inside its header")]
    Truncated,
    /// The track id is not a 32-bit value.
    #[error("the track id {0} does not fit 32 bits")]
    TrackIdOutOfRange(u64),
    /// The Opus payload is longer than
    /// [`MAX_AUDIO_PAYLOAD_LEN`].
    #[error("an audio payload of {len} bytes is longer than 1000 bytes")]
    PayloadTooLong {
        /// The length of the payload.
        len: usize,
    },
}

flags_type! {
    /// The `flags` byte of an audio datagram.
    AudioFlags {
        /// Bit 0: this is the last frame of a talkspurt.
        END_OF_TALKSPURT = 0;
        /// Bit 1: the payload is end-to-end encrypted. Reserved for a later
        /// minor version (section 6.5); a receiver that sees it and has no
        /// key must not hand the payload to the decoder.
        ENCRYPTED = 1;
    }
}

/// Why a keyframe is requested: the `reason` byte of a keyframe request. It
/// is for diagnostics; a receiver treats every reason, known or not, as a
/// request.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct KeyframeReason(pub u8);

impl KeyframeReason {
    /// 0: no reason given.
    pub const UNSPECIFIED: Self = Self(0);
    /// 1: a frame was lost, or the decoder lost its reference.
    pub const LOSS: Self = Self(1);
    /// 2: a viewer subscribed and needs a first keyframe.
    pub const NEW_VIEWER: Self = Self(2);
    /// 3: a viewer the server held back after dropping frames can resume.
    pub const BACKLOG_RECOVERY: Self = Self(3);
}

/// Reads the track id that follows the type byte. Returns it with the
/// number of bytes it took.
fn decode_track(src: &[u8]) -> Result<(TrackId, usize), DatagramError> {
    let (raw, len) = VarInt::decode(src).map_err(|_| DatagramError::Truncated)?;
    let track =
        u32::try_from(raw.get()).map_err(|_| DatagramError::TrackIdOutOfRange(raw.get()))?;
    Ok((TrackId(track), len))
}

/// The header of an audio datagram: everything before the Opus payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct AudioHeader {
    /// The track the frame belongs to.
    pub track: TrackId,
    /// The sequence number of the frame within its track; it wraps.
    pub seq: u16,
    /// The capture time of the frame in samples at 48 kHz; it wraps.
    pub timestamp: u32,
    /// The flags.
    pub flags: AudioFlags,
    /// The audio level of the frame in -dBov: 0 is the loudest, 127 is
    /// silence. The server uses it for speaking indicators and to pick the
    /// loudest speakers without decoding anything. A value above 127 reads
    /// as 127: see [`AudioHeader::level_dbov`].
    pub level: u8,
}

impl AudioHeader {
    /// The length of the longest header: type, an 8-byte track id and the
    /// fixed fields.
    pub const MAX_LEN: usize = 1 + VarInt::MAX_LEN + AUDIO_FIXED_LEN;

    /// The length of this header on the wire.
    #[must_use]
    pub const fn encoded_len(&self) -> usize {
        1 + VarInt::from_u32(self.track.0).encoded_len() + AUDIO_FIXED_LEN
    }

    /// The audio level in -dBov, between 0 and 127.
    #[must_use]
    pub const fn level_dbov(&self) -> u8 {
        if self.level > LEVEL_SILENCE {
            LEVEL_SILENCE
        } else {
            self.level
        }
    }

    /// Appends the header to `dst`, type byte first. The Opus payload goes
    /// right behind it.
    ///
    /// Panics, like every [`BufMut`] writer, when a fixed-size buffer has
    /// fewer than [`AudioHeader::encoded_len`] bytes left.
    pub fn encode<B: BufMut>(&self, dst: &mut B) {
        dst.put_u8(TYPE_AUDIO);
        VarInt::from_u32(self.track.0).encode(dst);
        dst.put_u16_le(self.seq);
        dst.put_u32_le(self.timestamp);
        dst.put_u8(self.flags.bits());
        dst.put_u8(self.level);
    }
}

/// An audio datagram: its header and its Opus payload, borrowed from the
/// datagram.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AudioDatagram<'a> {
    /// The header.
    pub header: AudioHeader,
    /// The Opus payload: the rest of the datagram, at most
    /// [`MAX_AUDIO_PAYLOAD_LEN`] bytes. It may be empty.
    pub payload: &'a [u8],
}

impl AudioDatagram<'_> {
    /// Appends the whole datagram to `dst`: the header and a copy of the
    /// payload. A sender that encodes Opus straight into its send buffer
    /// uses [`AudioHeader::encode`] instead and avoids the copy.
    ///
    /// Fails, and writes nothing, when the payload is too long.
    pub fn encode<B: BufMut>(&self, dst: &mut B) -> Result<(), DatagramError> {
        if self.payload.len() > MAX_AUDIO_PAYLOAD_LEN {
            return Err(DatagramError::PayloadTooLong {
                len: self.payload.len(),
            });
        }
        self.header.encode(dst);
        dst.put_slice(self.payload);
        Ok(())
    }
}

/// A keyframe request: a viewer asks the server, or the server asks the
/// publisher, for a keyframe of a video track. Best effort and rate limited
/// (section 6.4).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct KeyframeRequest {
    /// The video track.
    pub track: TrackId,
    /// Why the keyframe is needed.
    pub reason: KeyframeReason,
}

impl KeyframeRequest {
    /// The length of the longest request: type, an 8-byte track id and the
    /// reason.
    pub const MAX_LEN: usize = 1 + VarInt::MAX_LEN + 1;

    /// The length of this request on the wire.
    #[must_use]
    pub const fn encoded_len(&self) -> usize {
        1 + VarInt::from_u32(self.track.0).encoded_len() + 1
    }

    /// Appends the datagram to `dst`.
    ///
    /// Panics, like every [`BufMut`] writer, when a fixed-size buffer has
    /// fewer than [`KeyframeRequest::encoded_len`] bytes left.
    pub fn encode<B: BufMut>(&self, dst: &mut B) {
        dst.put_u8(TYPE_KEYFRAME_REQUEST);
        VarInt::from_u32(self.track.0).encode(dst);
        dst.put_u8(self.reason.0);
    }
}

/// A received datagram.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Datagram<'a> {
    /// Type `0x01`.
    Audio(AudioDatagram<'a>),
    /// Type `0x02`.
    KeyframeRequest(KeyframeRequest),
    /// A type this protocol version does not know. The datagram is ignored.
    Unknown(u8),
}

impl<'a> Datagram<'a> {
    /// Decodes a datagram. The payload of an audio datagram is borrowed from
    /// `datagram`, never copied. Never panics.
    ///
    /// Bytes after the `reason` of a keyframe request are ignored, so that a
    /// later minor version can append to it.
    pub fn decode(datagram: &'a [u8]) -> Result<Self, DatagramError> {
        let (&datagram_type, rest) = datagram.split_first().ok_or(DatagramError::Empty)?;
        match datagram_type {
            TYPE_AUDIO => {
                let (track, track_len) = decode_track(rest)?;
                let rest = &rest[track_len..];
                let (fixed, payload) = rest
                    .split_first_chunk::<AUDIO_FIXED_LEN>()
                    .ok_or(DatagramError::Truncated)?;
                if payload.len() > MAX_AUDIO_PAYLOAD_LEN {
                    return Err(DatagramError::PayloadTooLong { len: payload.len() });
                }
                let [seq0, seq1, ts0, ts1, ts2, ts3, flags, level] = *fixed;
                Ok(Self::Audio(AudioDatagram {
                    header: AudioHeader {
                        track,
                        seq: u16::from_le_bytes([seq0, seq1]),
                        timestamp: u32::from_le_bytes([ts0, ts1, ts2, ts3]),
                        flags: AudioFlags::from_bits_truncate(flags),
                        level,
                    },
                    payload,
                }))
            }
            TYPE_KEYFRAME_REQUEST => {
                let (track, track_len) = decode_track(rest)?;
                let reason = *rest.get(track_len).ok_or(DatagramError::Truncated)?;
                Ok(Self::KeyframeRequest(KeyframeRequest {
                    track,
                    reason: KeyframeReason(reason),
                }))
            }
            unknown => Ok(Self::Unknown(unknown)),
        }
    }
}
