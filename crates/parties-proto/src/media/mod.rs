//! The media plane (`docs/rust-rewrite/03-protocol-v2.md` section 6): the
//! layouts of datagrams and of video frame streams.
//!
//! Media headers are fixed binary layouts, little-endian, except the track
//! id, which is a QUIC variable-length integer. Reserved bytes and flag bits
//! are sent as zero and ignored on receipt (section 11): the types here
//! cannot hold a reserved bit, so an encoder cannot send one.
//!
//! The layouts are the same in both directions. The server checks a header
//! and forwards **the same bytes** to every recipient; it never rewrites a
//! media packet. Decoding therefore never copies a payload: a decoder
//! returns the header and where the payload starts, and the buffer stays
//! with the caller.
//!
//! | Module | Section | What it holds |
//! |---|---|---|
//! | [`datagram`] | 6.1 | Audio and keyframe-request datagrams |
//! | [`video`] | 6.3 | The header of a video frame stream |

pub mod datagram;
pub mod video;

pub use datagram::{
    AudioDatagram, AudioFlags, AudioHeader, Datagram, DatagramError, KeyframeReason,
    KeyframeRequest,
};
pub use video::{
    VideoCodecId, VideoFlags, VideoFrameHeader, VideoLayer, VideoStreamError, VideoStreamHeader,
};

/// Defines a set of flag bits in one byte. Bits outside `KNOWN` are
/// reserved: they are dropped when a byte is read and can never be set.
macro_rules! flags_type {
    (
        $(#[$meta:meta])*
        $name:ident {
            $( $(#[$flag_meta:meta])* $flag:ident = $bit:literal; )+
        }
    ) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
        pub struct $name(u8);

        impl $name {
            /// No flag.
            pub const EMPTY: Self = Self(0);

            $(
                $(#[$flag_meta])*
                pub const $flag: Self = Self(1 << $bit);
            )+

            /// Every flag this protocol version defines.
            pub const KNOWN: Self = Self(0 $(| (1 << $bit))+);

            /// Reads the flags byte of a header. Reserved bits are ignored.
            #[must_use]
            pub const fn from_bits_truncate(bits: u8) -> Self {
                Self(bits & Self::KNOWN.0)
            }

            /// The flags byte to send. Reserved bits are zero.
            #[must_use]
            pub const fn bits(self) -> u8 {
                self.0
            }

            /// Whether no flag is set.
            #[must_use]
            pub const fn is_empty(self) -> bool {
                self.0 == 0
            }

            /// Whether every flag of `other` is set in `self`.
            #[must_use]
            pub const fn contains(self, other: Self) -> bool {
                self.0 & other.0 == other.0
            }

            /// The flags set in `self` or in `other`.
            #[must_use]
            pub const fn union(self, other: Self) -> Self {
                Self(self.0 | other.0)
            }

            /// `self` with the flags of `other` set or cleared.
            #[must_use]
            pub const fn with(self, other: Self, set: bool) -> Self {
                if set {
                    Self(self.0 | other.0)
                } else {
                    Self(self.0 & !other.0)
                }
            }
        }

        impl core::ops::BitOr for $name {
            type Output = Self;

            fn bitor(self, rhs: Self) -> Self {
                self.union(rhs)
            }
        }

        impl core::fmt::Debug for $name {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                let mut first = true;
                write!(f, "{}(", stringify!($name))?;
                $(
                    if self.contains(Self::$flag) {
                        if !first {
                            f.write_str(" | ")?;
                        }
                        f.write_str(stringify!($flag))?;
                        first = false;
                    }
                )+
                let _ = first;
                f.write_str(")")
            }
        }
    };
}

pub(crate) use flags_type;
