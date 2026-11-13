//! Fingerprints: the short forms of a public key that people compare.
//!
//! A fingerprint is `SHA-256(public key)`, 32 bytes. It has two text forms:
//!
//! | Form | Example | Used for |
//! |---|---|---|
//! | Colon-separated hex ([`Fingerprint`]'s `Display`) | `fe:81:2c:...:38:89`, 95 characters | User identities. Identical to `public_key_fingerprint` of the C++ client, so the fingerprints servers and users know stay valid |
//! | Six words ([`SixWords`]) | `write annual again trap regular infant` | Server identities (`docs/rust-rewrite/04-connection-security.md` section 4) |
//!
//! # The six words
//!
//! The first 66 bits of the fingerprint, most significant bit first, are cut
//! into six groups of 11 bits. Each group is the index of a word in the
//! BIP-39 English list ([`crate::wordlist`]). The words are written in
//! lowercase, separated by single spaces.
//!
//! Six words carry 66 bits. That is enough to notice a wrong server when two
//! people compare the words, but it is not a substitute for the key: an
//! attacker who can afford about 2^66 key generations can make a key with
//! the same words. A join link carries the whole key and is the preferred
//! way to pin a server.

use core::fmt;
use core::str::FromStr;

use sha2::{Digest, Sha256};

use crate::encoding::{self, LengthError};
use crate::keys::PublicKey;
use crate::wordlist;

/// Length of a fingerprint in bytes.
pub const FINGERPRINT_LEN: usize = 32;

/// Length of the colon-separated hex form in characters.
pub const FINGERPRINT_TEXT_LEN: usize = FINGERPRINT_LEN * 3 - 1;

/// Number of words in the short form of a server identity.
pub const SIX_WORDS: usize = 6;

/// Why a text was not accepted as a fingerprint or as six words.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum FingerprintError {
    /// The hex form does not have 95 characters.
    #[error("fingerprint: {0}")]
    Length(#[from] LengthError),
    /// A character of the hex form is not where a hex digit or a colon
    /// belongs.
    #[error("the fingerprint is not colon-separated hex")]
    Encoding,
    /// The word form does not have exactly six words.
    #[error("expected six words, found {found}")]
    WordCount {
        /// How many words were given.
        found: usize,
    },
    /// A word is not in the list.
    #[error("word {position} is not in the word list")]
    UnknownWord {
        /// Position of the word, counted from 1.
        position: usize,
    },
}

/// The SHA-256 of a public key.
///
/// `Display` writes the 1.x form: 32 lowercase hex pairs separated by
/// colons. `FromStr` reads it back and also accepts uppercase digits.
///
/// ```
/// use parties_identity::{Fingerprint, SigningKey};
///
/// let fingerprint = SigningKey::from_seed(&[7; 32]).fingerprint();
/// let text = fingerprint.to_string();
/// assert_eq!(text.len(), 95);
/// assert_eq!(text.parse::<Fingerprint>(), Ok(fingerprint));
/// ```
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Fingerprint([u8; FINGERPRINT_LEN]);

impl Fingerprint {
    /// The fingerprint of a public key.
    #[must_use]
    pub fn of(key: &PublicKey) -> Self {
        Self(Sha256::digest(key.as_bytes()).into())
    }

    /// Wraps 32 bytes that are already a fingerprint, for example from a
    /// database.
    #[must_use]
    pub const fn from_bytes(bytes: [u8; FINGERPRINT_LEN]) -> Self {
        Self(bytes)
    }

    /// The 32 bytes of the fingerprint.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; FINGERPRINT_LEN] {
        &self.0
    }

    /// The six-word form.
    #[must_use]
    pub fn words(&self) -> SixWords {
        let mut prefix = 0u128;
        for &byte in &self.0[..9] {
            prefix = (prefix << 8) | u128::from(byte);
        }
        // `prefix` holds the first 72 bits; the words are its top 66.
        let mut indices = [0u16; SIX_WORDS];
        for (position, index) in indices.iter_mut().enumerate() {
            let shift = 72 - 11 * (position + 1);
            // The mask keeps 11 bits, which always fit.
            *index = ((prefix >> shift) & 0x7FF) as u16;
        }
        SixWords(indices)
    }
}

impl fmt::Display for Fingerprint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use fmt::Write as _;

        for (position, &byte) in self.0.iter().enumerate() {
            if position > 0 {
                f.write_char(':')?;
            }
            let [high, low] = encoding::hex_pair(byte);
            f.write_char(high)?;
            f.write_char(low)?;
        }
        Ok(())
    }
}

impl fmt::Debug for Fingerprint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Fingerprint({self})")
    }
}

impl FromStr for Fingerprint {
    type Err = FingerprintError;

    fn from_str(text: &str) -> Result<Self, FingerprintError> {
        let characters = text.as_bytes();
        if characters.len() != FINGERPRINT_TEXT_LEN {
            return Err(FingerprintError::Length(LengthError {
                expected: FINGERPRINT_TEXT_LEN,
                found: characters.len(),
            }));
        }
        let mut bytes = [0u8; FINGERPRINT_LEN];
        for (position, byte) in bytes.iter_mut().enumerate() {
            let at = position * 3;
            if position > 0 && characters[at - 1] != b':' {
                return Err(FingerprintError::Encoding);
            }
            let high = encoding::hex_value(characters[at]).ok_or(FingerprintError::Encoding)?;
            let low = encoding::hex_value(characters[at + 1]).ok_or(FingerprintError::Encoding)?;
            *byte = (high << 4) | low;
        }
        Ok(Self(bytes))
    }
}

/// The six-word form of a fingerprint: its first 66 bits.
///
/// `Display` writes the words in lowercase with single spaces. `FromStr`
/// accepts any mix of case, and spaces, line breaks or hyphens between the
/// words.
///
/// ```
/// use parties_identity::{SigningKey, SixWords};
///
/// let server = SigningKey::from_seed(&[7; 32]).public_key();
/// let words = server.fingerprint().words();
/// assert_eq!(words.to_string(), "write annual again trap regular infant");
///
/// // What someone read out over the phone and the user typed in.
/// let typed: SixWords = "Write-Annual-Again-Trap-Regular-Infant".parse()?;
/// assert!(typed.matches(&server.fingerprint()));
/// # Ok::<(), parties_identity::FingerprintError>(())
/// ```
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct SixWords([u16; SIX_WORDS]);

impl SixWords {
    /// The six words, in order.
    #[must_use]
    pub fn words(&self) -> [&'static str; SIX_WORDS] {
        // Every index is below 2048 by construction.
        self.0
            .map(|index| wordlist::word(index).unwrap_or_default())
    }

    /// The six word indices, each below 2048.
    #[must_use]
    pub const fn indices(&self) -> [u16; SIX_WORDS] {
        self.0
    }

    /// Whether these are the six words of the given fingerprint.
    #[must_use]
    pub fn matches(&self, fingerprint: &Fingerprint) -> bool {
        *self == fingerprint.words()
    }
}

impl fmt::Display for SixWords {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (position, word) in self.words().into_iter().enumerate() {
            if position > 0 {
                f.write_str(" ")?;
            }
            f.write_str(word)?;
        }
        Ok(())
    }
}

impl fmt::Debug for SixWords {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SixWords({self})")
    }
}

impl FromStr for SixWords {
    type Err = FingerprintError;

    fn from_str(text: &str) -> Result<Self, FingerprintError> {
        let mut indices = [0u16; SIX_WORDS];
        let mut found = 0usize;
        let words = text
            .split(|character: char| character.is_whitespace() || character == '-')
            .filter(|word| !word.is_empty());
        for word in words {
            let index = wordlist::index_of(word).ok_or(FingerprintError::UnknownWord {
                position: found + 1,
            })?;
            if let Some(slot) = indices.get_mut(found) {
                *slot = index;
            }
            found += 1;
        }
        if found == SIX_WORDS {
            Ok(Self(indices))
        } else {
            Err(FingerprintError::WordCount { found })
        }
    }
}
