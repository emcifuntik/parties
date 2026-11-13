//! Home: the user's parties and their state, the identity card, and the
//! "Add a party" card.

use super::{Avatar, PartyIcon, PartyId};

/// What the Home view shows besides the user's own profile, which is
/// [`SelfState`](super::SelfState).
#[derive(Clone, Debug, PartialEq)]
pub struct Home {
    pub parties: Vec<PartyCard>,
    pub identity: IdentityCard,
    /// The text in the join code field.
    pub join_code: String,
}

/// One row of "Your parties".
#[derive(Clone, Debug, PartialEq)]
pub struct PartyCard {
    pub id: PartyId,
    pub name: String,
    pub icon: PartyIcon,
    /// Host and port, as the party was added.
    pub address: String,
    /// The party only admits members with a join code ("Private").
    pub locked: bool,
    pub link: PartyLink,
    /// The voice rooms with people in them. Known for a connected party.
    pub rooms: Vec<RoomPreview>,
}

/// How the client stands with a party.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PartyLink {
    /// The client is connected to it.
    Connected {
        online: u32,
        capacity: u32,
        latency_ms: u32,
    },
    /// It answers the server query; the client is not connected to it.
    Reachable {
        online: u32,
        capacity: u32,
        latency_ms: u32,
    },
    /// It does not answer. `last_seen` is ready to show: "3 days ago".
    Offline { last_seen: String },
    /// The connection dropped and the client is trying again.
    Reconnecting { attempt: u32 },
}

impl PartyLink {
    /// The party answers right now.
    pub const fn is_online(&self) -> bool {
        matches!(self, Self::Connected { .. } | Self::Reachable { .. })
    }

    pub const fn is_connected(&self) -> bool {
        matches!(self, Self::Connected { .. })
    }
}

/// A voice room with people in it, as a party row previews it.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomPreview {
    pub name: String,
    /// The first few people of the room, for the row of pictures.
    pub avatars: Vec<Avatar>,
    /// Everyone in the room, the ones without a picture here included.
    pub people: u32,
}

/// The user's key, as Home shows it under the profile.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IdentityCard {
    /// The short form of the key fingerprint: "5E:7A:91:C2:4D:3F:08:B1".
    pub fingerprint: String,
    pub key: KeyStorage,
}

/// Where the identity key is kept.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyStorage {
    /// In the operating system's keystore on this device.
    Device,
    /// On a hardware key; every sign-in asks for it.
    HardwareKey,
    /// In a passkey, with a copy in this device's keystore.
    Passkey,
}

impl KeyStorage {
    /// The line under the fingerprint.
    pub const fn label(self) -> &'static str {
        match self {
            Self::Device => "Key kept on this device",
            Self::HardwareKey => "Signed in with a hardware key",
            Self::Passkey => "Key kept in a passkey",
        }
    }
}

/// A downloaded update that a restart installs. The title bar shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpdateNotice {
    pub version: String,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_party_is_online_when_it_answers() {
        let online = PartyLink::Reachable {
            online: 7,
            capacity: 32,
            latency_ms: 41,
        };
        assert!(online.is_online());
        assert!(!online.is_connected());
        assert!(!PartyLink::Reconnecting { attempt: 3 }.is_online());
        assert!(
            !PartyLink::Offline {
                last_seen: "3 days ago".to_owned()
            }
            .is_online()
        );
    }
}
