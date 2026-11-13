//! The user's own pages of Settings: Profile (picture and names) and Keys
//! (the keys that sign the user in).

use super::{PartyIcon, PartyId};

/// What the Profile page shows and edits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Profile {
    /// What is known about the picture file, ready to show:
    /// "256 \u{d7} 256, JPEG, 41 KB". `None` when the user has no picture.
    pub picture: Option<String>,
    /// The text in the display name field. It becomes the user's name when
    /// the field is committed.
    pub display_name: String,
    /// The name the user goes by in each party.
    pub party_names: Vec<PartyName>,
}

/// The name the user goes by in one party.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartyName {
    pub party: PartyId,
    pub party_name: String,
    pub icon: PartyIcon,
    /// The client is connected to this party.
    pub connected: bool,
    /// A name for this party only; `None` uses the display name.
    pub nickname: Option<String>,
}

/// What the Keys page shows and edits, besides the fingerprint (which is
/// [`IdentityCard`](super::IdentityCard)).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Keys {
    /// The keys that sign the user in.
    pub keys: Vec<KeyEntry>,
    /// On this device only a hardware key signs in.
    pub always_ask_hardware_key: bool,
}

impl Keys {
    /// Removes a key, unless it is the only one: losing the last key locks
    /// the user out.
    pub fn remove(&mut self, id: u32) -> bool {
        if self.keys.len() <= 1 || !self.keys.iter().any(|key| key.id == id) {
            return false;
        }
        self.keys.retain(|key| key.id != id);
        true
    }
}

/// One key of the user.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeyEntry {
    pub id: u32,
    pub kind: KeyKind,
    /// "Passkey \u{b7} Windows Hello", "YubiKey 5C NFC".
    pub name: String,
    /// When it was added and what is known of its use, ready to show.
    pub detail: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyKind {
    Passkey,
    HardwareKey,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(id: u32) -> KeyEntry {
        KeyEntry {
            id,
            kind: KeyKind::Passkey,
            name: format!("Key {id}"),
            detail: String::new(),
        }
    }

    #[test]
    fn the_last_key_cannot_be_removed() {
        let mut keys = Keys {
            keys: vec![key(1), key(2)],
            always_ask_hardware_key: false,
        };
        assert!(!keys.remove(7), "an unknown key removes nothing");
        assert!(keys.remove(1));
        assert_eq!(keys.keys, [key(2)]);
        assert!(!keys.remove(2));
        assert_eq!(keys.keys.len(), 1);
    }
}
