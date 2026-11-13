//! QUIC variable-length integers (RFC 9000 section 16).
//!
//! The protocol uses them for the stream type, for the length of a frame and
//! for the track id in media headers
//! (`docs/rust-rewrite/03-protocol-v2.md` sections 2, 3 and 6).
//!
//! The two most significant bits of the first byte give the length of the
//! encoding (1, 2, 4 or 8 bytes); the remaining bits hold the value in
//! network byte order. The encoder always writes the shortest form. The
//! decoder accepts every form, as RFC 9000 does: a value may be encoded on
//! more bytes than it needs.

use core::fmt;

use bytes::{Buf, BufMut};
use thiserror::Error;

/// An integer a QUIC variable-length integer can hold: 0 to 2^62 - 1.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct VarInt(u64);

/// The value is above [`VarInt::MAX`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("{0} does not fit a QUIC variable-length integer (at most 2^62 - 1)")]
pub struct VarIntRangeError(pub u64);

/// The input ends before the integer does. Nothing was consumed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("a variable-length integer of {needed} bytes is cut off after {available}")]
pub struct VarIntIncomplete {
    /// The length of the whole encoding. 1 when the input is empty, because
    /// the first byte is what says more.
    pub needed: usize,
    /// How many bytes the input holds.
    pub available: usize,
}

impl VarInt {
    /// The largest value: 2^62 - 1.
    pub const MAX: Self = Self((1 << 62) - 1);

    /// The length of the longest encoding, in bytes.
    pub const MAX_LEN: usize = 8;

    /// Builds a value. Fails when `value` is above [`VarInt::MAX`].
    pub const fn new(value: u64) -> Result<Self, VarIntRangeError> {
        if value <= Self::MAX.0 {
            Ok(Self(value))
        } else {
            Err(VarIntRangeError(value))
        }
    }

    /// Builds a value from 32 bits, which always fit.
    #[must_use]
    pub const fn from_u32(value: u32) -> Self {
        // Lossless: u32 to u64.
        Self(value as u64)
    }

    /// The value.
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0
    }

    /// The length of the shortest encoding of this value: 1, 2, 4 or 8.
    #[must_use]
    pub const fn encoded_len(self) -> usize {
        if self.0 < 1 << 6 {
            1
        } else if self.0 < 1 << 14 {
            2
        } else if self.0 < 1 << 30 {
            4
        } else {
            8
        }
    }

    /// The length of the encoding that starts with `first`: 1, 2, 4 or 8.
    #[must_use]
    pub const fn len_from_first_byte(first: u8) -> usize {
        1 << (first >> 6)
    }

    /// Appends the shortest encoding to `dst`.
    ///
    /// `dst` grows when it is a `BytesMut` or a `Vec<u8>`. Like every
    /// [`BufMut`] writer, this panics when a fixed-size buffer has fewer
    /// than [`VarInt::encoded_len`] bytes left.
    pub fn encode<B: BufMut>(self, dst: &mut B) {
        let len = self.encoded_len();
        // The length prefix is 0, 1, 2 or 3 in the two bits above the value.
        let prefix = u64::from(len.trailing_zeros());
        let tagged = self.0 | (prefix << (len * 8 - 2));
        dst.put_slice(&tagged.to_be_bytes()[8 - len..]);
    }

    /// The shortest encoding as an array, with its length. The bytes after
    /// the length are zero.
    #[must_use]
    pub fn to_bytes(self) -> ([u8; Self::MAX_LEN], usize) {
        let mut bytes = [0; Self::MAX_LEN];
        let mut cursor = &mut bytes[..];
        self.encode(&mut cursor);
        (bytes, self.encoded_len())
    }

    /// Reads one integer from the start of `src` and returns it with the
    /// number of bytes it took. Never panics.
    pub fn decode(src: &[u8]) -> Result<(Self, usize), VarIntIncomplete> {
        let incomplete = |needed| VarIntIncomplete {
            needed,
            available: src.len(),
        };
        let first = *src.first().ok_or(incomplete(1))?;
        let len = Self::len_from_first_byte(first);
        let encoded = src.get(..len).ok_or(incomplete(len))?;
        let mut bytes = [0; 8];
        bytes[8 - len..].copy_from_slice(encoded);
        bytes[8 - len] &= 0x3F;
        Ok((Self(u64::from_be_bytes(bytes)), len))
    }

    /// Reads one integer from `src` and advances it. When the integer is
    /// incomplete, `src` is left as it was.
    pub fn decode_buf<B: Buf>(src: &mut B) -> Result<Self, VarIntIncomplete> {
        let available = src.remaining();
        let first = *src.chunk().first().ok_or(VarIntIncomplete {
            needed: 1,
            available,
        })?;
        let len = Self::len_from_first_byte(first);
        if available < len {
            return Err(VarIntIncomplete {
                needed: len,
                available,
            });
        }
        let mut bytes = [0; 8];
        src.copy_to_slice(&mut bytes[8 - len..]);
        bytes[8 - len] &= 0x3F;
        Ok(Self(u64::from_be_bytes(bytes)))
    }
}

impl fmt::Display for VarInt {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl From<u8> for VarInt {
    fn from(value: u8) -> Self {
        Self(u64::from(value))
    }
}

impl From<u16> for VarInt {
    fn from(value: u16) -> Self {
        Self(u64::from(value))
    }
}

impl From<u32> for VarInt {
    fn from(value: u32) -> Self {
        Self(u64::from(value))
    }
}

impl TryFrom<u64> for VarInt {
    type Error = VarIntRangeError;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl TryFrom<usize> for VarInt {
    type Error = VarIntRangeError;

    fn try_from(value: usize) -> Result<Self, Self::Error> {
        // A `usize` wider than 64 bits does not exist on a supported target;
        // if it did, such a value would be out of range anyway.
        Self::new(u64::try_from(value).unwrap_or(u64::MAX))
    }
}

impl From<VarInt> for u64 {
    fn from(value: VarInt) -> Self {
        value.0
    }
}
