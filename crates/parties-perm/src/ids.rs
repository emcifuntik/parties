//! Identifier newtypes used by the permission engine.
//!
//! These types belong in `parties-proto`
//! (`docs/rust-rewrite/03-protocol-v2.md` section 5.2: users, roles, channels
//! and messages are identified by a server-assigned `u64`). That crate does
//! not exist yet, so the few identifiers the engine needs are defined here.
//! When `parties-proto` lands, this module becomes a re-export and nothing
//! else in the crate changes.

macro_rules! id_type {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name(pub u64);

        impl $name {
            /// Returns the raw 64-bit value.
            #[must_use]
            pub const fn get(self) -> u64 {
                self.0
            }
        }

        impl From<u64> for $name {
            fn from(raw: u64) -> Self {
                Self(raw)
            }
        }

        impl From<$name> for u64 {
            fn from(id: $name) -> Self {
                id.0
            }
        }
    };
}

id_type! {
    /// Identifies a user (and therefore a member) of the server.
    UserId
}

id_type! {
    /// Identifies a role, including the built-in `@everyone` role.
    RoleId
}

id_type! {
    /// Identifies a channel of any kind (category, voice, text).
    ChannelId
}

id_type! {
    /// Identifies a chat message. Only used by [`crate::Scope::ChannelOf`].
    MessageId
}
