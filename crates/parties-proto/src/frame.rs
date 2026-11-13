//! Framing of control and bulk streams
//! (`docs/rust-rewrite/03-protocol-v2.md` section 3).
//!
//! A stream carries frames of `[length varint][protobuf message]`. The
//! length counts the bytes of the message only and may be 0: a message whose
//! fields all have their default value encodes to nothing.
//!
//! A frame longer than the limit of its stream, or one that does not decode,
//! is a protocol violation: the receiver closes the connection with
//! [`CloseCode::ProtocolViolation`]. There is no resynchronisation, so after
//! an error the codec must not be used on that stream again. The two cases
//! are distinct errors, [`FrameError::TooLong`] and
//! [`FrameError::Malformed`].
//!
//! The limit is checked on the length prefix, before a single byte of the
//! message is buffered: a receiver never allocates for a length it has not
//! checked.
//!
//! # Example
//!
//! ```
//! use bytes::BytesMut;
//! use parties_proto::frame::FrameCodec;
//! use parties_proto::v2::{ClientFrame, LeaveVoice, client_frame::Body};
//!
//! let request = ClientFrame {
//!     request_id: 7,
//!     reason: String::new(),
//!     body: Some(Body::LeaveVoice(LeaveVoice {})),
//! };
//!
//! let codec = FrameCodec::CONTROL;
//! let mut wire = BytesMut::new();
//! codec.encode(&request, &mut wire)?;
//!
//! // The receiver appends what it reads to a buffer and takes frames out
//! // of it as they become complete.
//! let mut received = BytesMut::new();
//! received.extend_from_slice(&wire[..2]);
//! assert_eq!(codec.decode_message::<ClientFrame>(&mut received)?, None);
//! received.extend_from_slice(&wire[2..]);
//! assert_eq!(codec.decode_message::<ClientFrame>(&mut received)?, Some(request));
//! # Ok::<(), parties_proto::frame::FrameError>(())
//! ```

use bytes::{Buf, Bytes, BytesMut};
use prost::Message;
use thiserror::Error;

use crate::codes::CloseCode;
use crate::limits::{MAX_BULK_FRAME_LEN, MAX_CONTROL_FRAME_LEN, MAX_CONTROL_FRAME_LEN_PRE_AUTH};
use crate::varint::VarInt;

/// Why a frame was refused. Both cases are protocol violations.
#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum FrameError {
    /// The frame is longer than the limit of its stream.
    #[error("a frame of {len} bytes is longer than the limit of {max} bytes")]
    TooLong {
        /// The length the frame announces, or the encoded length of the
        /// message that was to be sent.
        len: u64,
        /// The limit of the stream.
        max: usize,
    },
    /// The frame is not a valid encoding of the expected message.
    #[error("a frame does not decode: {0}")]
    Malformed(#[from] prost::DecodeError),
}

impl FrameError {
    /// The code to close the connection with:
    /// [`CloseCode::ProtocolViolation`].
    #[must_use]
    pub const fn close_code(&self) -> CloseCode {
        CloseCode::ProtocolViolation
    }
}

/// Looks for a complete frame at the start of `src` without consuming it.
///
/// Returns the length of the length prefix and the length of the message,
/// or `Ok(None)` when `src` does not hold the whole frame yet. Fails as soon
/// as the length prefix announces more than `max_len` bytes, however little
/// of the message has arrived.
pub fn peek_frame(src: &[u8], max_len: usize) -> Result<Option<(usize, usize)>, FrameError> {
    let Ok((len, prefix_len)) = VarInt::decode(src) else {
        return Ok(None);
    };
    let too_long = FrameError::TooLong {
        len: len.get(),
        max: max_len,
    };
    let Ok(len) = usize::try_from(len.get()) else {
        return Err(too_long);
    };
    if len > max_len {
        return Err(too_long);
    }
    if src.len() - prefix_len < len {
        return Ok(None);
    }
    Ok(Some((prefix_len, len)))
}

/// Writes and reads the frames of one stream, with the stream's limit.
///
/// The codec holds no buffer and no state besides the limit. The caller
/// owns the receive buffer, appends what it reads from the stream, and
/// calls [`FrameCodec::decode`] until it returns `Ok(None)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameCodec {
    max_len: usize,
}

impl FrameCodec {
    /// The control stream before authentication: 4 KiB.
    pub const CONTROL_PRE_AUTH: Self = Self::new(MAX_CONTROL_FRAME_LEN_PRE_AUTH);

    /// The control stream after authentication: 64 KiB.
    pub const CONTROL: Self = Self::new(MAX_CONTROL_FRAME_LEN);

    /// A bulk stream: 64 KiB.
    pub const BULK: Self = Self::new(MAX_BULK_FRAME_LEN);

    /// A codec whose frames carry at most `max_len` bytes of message.
    #[must_use]
    pub const fn new(max_len: usize) -> Self {
        Self { max_len }
    }

    /// The longest message a frame may carry, in bytes.
    #[must_use]
    pub const fn max_len(&self) -> usize {
        self.max_len
    }

    /// Changes the limit, for example when authentication succeeds.
    pub const fn set_max_len(&mut self, max_len: usize) {
        self.max_len = max_len;
    }

    /// Takes the next frame out of `src` and returns its message bytes,
    /// without copying them.
    ///
    /// Returns `Ok(None)` when `src` does not hold a whole frame yet; `src`
    /// is then left as it was, and the caller reads more from the stream.
    pub fn decode(&self, src: &mut BytesMut) -> Result<Option<Bytes>, FrameError> {
        let Some((prefix_len, len)) = peek_frame(src, self.max_len)? else {
            return Ok(None);
        };
        src.advance(prefix_len);
        Ok(Some(src.split_to(len).freeze()))
    }

    /// Takes the next frame out of `src` and decodes it as `M`.
    ///
    /// `bytes` fields of the message are slices of the frame, not copies.
    pub fn decode_message<M>(&self, src: &mut BytesMut) -> Result<Option<M>, FrameError>
    where
        M: Message + Default,
    {
        match self.decode(src)? {
            Some(frame) => Ok(Some(M::decode(frame)?)),
            None => Ok(None),
        }
    }

    /// Appends `message` to `dst` as one frame.
    ///
    /// Fails with [`FrameError::TooLong`], and writes nothing, when the
    /// encoded message is longer than the limit.
    pub fn encode<M>(&self, message: &M, dst: &mut BytesMut) -> Result<(), FrameError>
    where
        M: Message,
    {
        let len = message.encoded_len();
        let prefix = self.length_prefix(len)?;
        dst.reserve(prefix.encoded_len() + len);
        prefix.encode(dst);
        message.encode_raw(dst);
        Ok(())
    }

    /// Appends already encoded message bytes to `dst` as one frame.
    ///
    /// Fails with [`FrameError::TooLong`], and writes nothing, when
    /// `message` is longer than the limit.
    pub fn encode_bytes(&self, message: &[u8], dst: &mut BytesMut) -> Result<(), FrameError> {
        let prefix = self.length_prefix(message.len())?;
        dst.reserve(prefix.encoded_len() + message.len());
        prefix.encode(dst);
        dst.extend_from_slice(message);
        Ok(())
    }

    fn length_prefix(&self, len: usize) -> Result<VarInt, FrameError> {
        let too_long = FrameError::TooLong {
            // Lossless on every supported target; saturates otherwise.
            len: u64::try_from(len).unwrap_or(u64::MAX),
            max: self.max_len,
        };
        if len > self.max_len {
            return Err(too_long);
        }
        VarInt::try_from(len).map_err(|_| too_long)
    }
}
