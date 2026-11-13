//! Identifiers (`docs/rust-rewrite/03-protocol-v2.md` section 5.2).
//!
//! | Identifier | Type | Notes |
//! |---|---|---|
//! | User, role, channel, message, join code, attachment, audit entry | `u64`, time-sortable | [`TimeSortableId`]: 42-bit millisecond timestamp, 22-bit sequence |
//! | Session | `u64` | One per authenticated connection |
//! | Track | `u32` | The only identifier on the media plane |
//!
//! All of them are assigned by the server. 0 is never assigned: in a message
//! it stands for "none", which is also what an absent protobuf field reads
//! as.
//!
//! The generated messages carry identifiers as plain integers. The newtypes
//! here keep one kind of identifier from being passed where another is
//! expected.

use core::fmt;
use core::num::NonZeroU64;

use thiserror::Error;

/// The instant a [`TimeSortableId`] counts from: 2026-01-01T00:00:00Z, in
/// milliseconds since the Unix epoch.
///
/// With this epoch an identifier stays below 2^63 until the year 2095, so it
/// fits the signed 64-bit integer SQLite stores and sorts there in the order
/// it was assigned.
pub const EPOCH_UNIX_MS: u64 = 1_767_225_600_000;

/// The number of bits of the timestamp, the upper part of the identifier.
pub const TIMESTAMP_BITS: u32 = 42;

/// The number of bits of the sequence, the lower part of the identifier.
pub const SEQUENCE_BITS: u32 = 22;

/// A 64-bit time-sortable identifier: the upper 42 bits are a timestamp in
/// milliseconds since [`EPOCH_UNIX_MS`], the lower 22 bits a sequence number
/// that tells apart the identifiers assigned in the same millisecond.
///
/// Identifiers compare in the order of their timestamps, then of their
/// sequence numbers. The value is never 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TimeSortableId(NonZeroU64);

/// Why [`TimeSortableId::new`] or [`TimeSortableId::from_unix_ms`] refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum IdError {
    /// The timestamp needs more than 42 bits.
    #[error("the timestamp {0} ms does not fit 42 bits")]
    TimestampOutOfRange(u64),
    /// The sequence number needs more than 22 bits.
    #[error("the sequence number {0} does not fit 22 bits")]
    SequenceOutOfRange(u32),
    /// The instant lies before [`EPOCH_UNIX_MS`].
    #[error("{0} ms since the Unix epoch is before the epoch of identifiers")]
    BeforeEpoch(u64),
    /// Timestamp 0 with sequence 0 would be the identifier 0, which stands
    /// for "none".
    #[error("the identifier 0 is never assigned")]
    Zero,
}

impl TimeSortableId {
    /// The largest timestamp: 2^42 - 1 milliseconds after the epoch.
    pub const MAX_TIMESTAMP_MS: u64 = (1 << TIMESTAMP_BITS) - 1;

    /// The largest sequence number: 2^22 - 1.
    pub const MAX_SEQUENCE: u32 = (1 << SEQUENCE_BITS) - 1;

    /// Builds an identifier from its parts: milliseconds since
    /// [`EPOCH_UNIX_MS`] and a sequence number.
    pub const fn new(timestamp_ms: u64, sequence: u32) -> Result<Self, IdError> {
        if timestamp_ms > Self::MAX_TIMESTAMP_MS {
            return Err(IdError::TimestampOutOfRange(timestamp_ms));
        }
        if sequence > Self::MAX_SEQUENCE {
            return Err(IdError::SequenceOutOfRange(sequence));
        }
        // Lossless: u32 to u64.
        match NonZeroU64::new((timestamp_ms << SEQUENCE_BITS) | sequence as u64) {
            Some(raw) => Ok(Self(raw)),
            None => Err(IdError::Zero),
        }
    }

    /// Builds an identifier for an instant given in milliseconds since the
    /// Unix epoch.
    pub const fn from_unix_ms(unix_ms: u64, sequence: u32) -> Result<Self, IdError> {
        match unix_ms.checked_sub(EPOCH_UNIX_MS) {
            Some(timestamp_ms) => Self::new(timestamp_ms, sequence),
            None => Err(IdError::BeforeEpoch(unix_ms)),
        }
    }

    /// Reads an identifier from its integer form. Returns `None` for 0,
    /// which stands for "none". Every other value has a timestamp and a
    /// sequence number.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Option<Self> {
        match NonZeroU64::new(raw) {
            Some(raw) => Some(Self(raw)),
            None => None,
        }
    }

    /// The integer form.
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0.get()
    }

    /// The timestamp: milliseconds since [`EPOCH_UNIX_MS`].
    #[must_use]
    pub const fn timestamp_ms(self) -> u64 {
        self.0.get() >> SEQUENCE_BITS
    }

    /// The timestamp as milliseconds since the Unix epoch.
    #[must_use]
    pub const fn unix_ms(self) -> u64 {
        // At most 2^42 - 1 plus the epoch: far from overflow.
        self.timestamp_ms() + EPOCH_UNIX_MS
    }

    /// The sequence number.
    #[must_use]
    pub const fn sequence(self) -> u32 {
        // The mask keeps 22 bits, so the value fits.
        (self.0.get() & Self::MAX_SEQUENCE as u64) as u32
    }
}

impl fmt::Display for TimeSortableId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl From<TimeSortableId> for u64 {
    fn from(id: TimeSortableId) -> Self {
        id.get()
    }
}

macro_rules! id_type {
    ($(#[$meta:meta])* $name:ident($raw:ty)) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name(pub $raw);

        impl $name {
            /// The value that stands for "none". The server never assigns
            /// it.
            pub const NONE: Self = Self(0);

            /// Returns the raw value.
            #[must_use]
            pub const fn get(self) -> $raw {
                self.0
            }

            /// Whether this is [`Self::NONE`].
            #[must_use]
            pub const fn is_none(self) -> bool {
                self.0 == 0
            }
        }

        impl From<$raw> for $name {
            fn from(raw: $raw) -> Self {
                Self(raw)
            }
        }

        impl From<$name> for $raw {
            fn from(id: $name) -> Self {
                id.0
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.fmt(f)
            }
        }
    };
}

macro_rules! time_sortable_id_type {
    ($(#[$meta:meta])* $name:ident) => {
        id_type! {
            $(#[$meta])*
            $name(u64)
        }

        impl $name {
            /// The timestamp and the sequence number of the identifier, or
            /// `None` for [`Self::NONE`].
            #[must_use]
            pub const fn time_sortable(self) -> Option<TimeSortableId> {
                TimeSortableId::from_raw(self.0)
            }
        }

        impl From<TimeSortableId> for $name {
            fn from(id: TimeSortableId) -> Self {
                Self(id.get())
            }
        }
    };
}

time_sortable_id_type! {
    /// Identifies a user (and therefore a member) of the server.
    UserId
}

time_sortable_id_type! {
    /// Identifies a role, including the built-in `@everyone` role.
    RoleId
}

time_sortable_id_type! {
    /// Identifies a channel of any kind (category, voice, text).
    ChannelId
}

time_sortable_id_type! {
    /// Identifies a chat message.
    MessageId
}

time_sortable_id_type! {
    /// Identifies a join code. The code itself is a secret; this is not.
    JoinCodeId
}

time_sortable_id_type! {
    /// Identifies an uploaded file.
    AttachmentId
}

time_sortable_id_type! {
    /// Identifies an entry of the audit log.
    AuditEntryId
}

id_type! {
    /// Identifies one authenticated connection. A user may have several.
    SessionId(u64)
}

id_type! {
    /// Identifies one published media track. It is the only identifier on
    /// the media plane, where it travels as a QUIC variable-length integer.
    TrackId(u32)
}
