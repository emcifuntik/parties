//! Server trust: pins and rotation statements
//! (`docs/rust-rewrite/04-connection-security.md` section 4).
//!
//! A client trusts a server by its identity key. The key it expects is the
//! **pin**; it comes from a join link, from an earlier connection, or from
//! the user confirming the server's six words. [`check_pin`] is the one
//! comparison every connection goes through.
//!
//! # Rotation statements
//!
//! A server that changes its identity key presents a rotation statement: the
//! new key signed by the old key.
//!
//! ```text
//! signature = Sign(old_key, "parties-server-rotation-v1" || 0x00 || old_key || new_key)
//! ```
//!
//! On the wire and on disk a statement is `old_key || new_key || signature`,
//! 128 bytes. A server that rotated more than once presents all its
//! statements, one after the other, and a client follows them from its pin
//! to the key the server shows now: at most [`MAX_ROTATION_STATEMENTS`]
//! statements in a list, and at most [`MAX_ROTATION_CHAIN`] of them in a
//! row.
//!
//! A rotation statement is as trustworthy as the old key. It serves a
//! planned change of key; it is no defence against a stolen old key, whose
//! holder can sign a statement too. A server whose key was stolen has to be
//! pinned again by hand.

use crate::encoding::{self, LengthError};
use crate::keys::{
    InvalidSignature, KeyError, PUBLIC_KEY_LEN, PublicKey, SIGNATURE_LEN, Signature, SigningKey,
};

/// Domain label of a rotation statement.
pub const ROTATION_LABEL: &[u8] = b"parties-server-rotation-v1";

/// Length of the message a rotation statement signs.
pub const ROTATION_MESSAGE_LEN: usize = ROTATION_LABEL.len() + 1 + 2 * PUBLIC_KEY_LEN;

/// Length of an encoded rotation statement: two keys and a signature.
pub const ROTATION_STATEMENT_LEN: usize = 2 * PUBLIC_KEY_LEN + SIGNATURE_LEN;

/// Longest chain of rotation statements [`check_pin`] follows from a pin.
pub const MAX_ROTATION_CHAIN: usize = 16;

/// Most statements [`RotationStatement::list_from_bytes`] accepts. Every
/// statement costs a signature check, so a peer's list is bounded before
/// any of it is verified.
pub const MAX_ROTATION_STATEMENTS: usize = 32;

/// Why a rotation statement was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum RotationError {
    /// The encoded statement does not have 128 bytes.
    #[error("rotation statement: {0}")]
    Length(#[from] LengthError),
    /// An encoded list is not a whole number of 128-byte statements.
    #[error("a list of rotation statements cannot have {found} bytes")]
    ListLength {
        /// The length that was given, in bytes.
        found: usize,
    },
    /// An encoded list has more than [`MAX_ROTATION_STATEMENTS`] statements.
    #[error("{found} rotation statements are too many")]
    TooMany {
        /// How many statements the list holds.
        found: usize,
    },
    /// One of the two keys is not a valid public key.
    #[error("rotation statement: {0}")]
    Key(#[from] KeyError),
    /// The old and the new key are the same.
    #[error("a rotation statement needs two different keys")]
    SameKey,
    /// The signature is not the old key's signature over this statement.
    #[error("rotation statement: {0}")]
    Signature(#[from] InvalidSignature),
}

/// The message a rotation statement signs:
/// `"parties-server-rotation-v1" || 0x00 || old_key || new_key`.
#[must_use]
pub fn rotation_message(old_key: &PublicKey, new_key: &PublicKey) -> [u8; ROTATION_MESSAGE_LEN] {
    encoding::concat(&[ROTATION_LABEL, &[0], old_key.as_bytes(), new_key.as_bytes()])
}

/// A verified rotation statement: the old key vouches for the new key.
///
/// Every value of this type carries a signature that was checked when the
/// value was made, so holding one is enough to follow it.
///
/// ```
/// use parties_identity::{PinCheck, RotationStatement, SigningKey, check_pin};
///
/// let old_key = SigningKey::from_seed(&[1; 32]);
/// let new_key = SigningKey::from_seed(&[2; 32]);
///
/// // On the server, once, when the key changes.
/// let statement = RotationStatement::sign(&old_key, &new_key.public_key())?;
/// let stored = statement.to_bytes();
///
/// // On a client that pinned the old key.
/// let statement = RotationStatement::from_bytes(&stored)?;
/// let pin = old_key.public_key();
/// assert_eq!(
///     check_pin(Some(&pin), &new_key.public_key(), &[statement]),
///     PinCheck::Rotated,
/// );
/// // Without the statement, the changed key is a hard failure.
/// assert_eq!(check_pin(Some(&pin), &new_key.public_key(), &[]), PinCheck::Mismatch);
/// # Ok::<(), parties_identity::RotationError>(())
/// ```
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RotationStatement {
    old_key: PublicKey,
    new_key: PublicKey,
    signature: Signature,
}

impl RotationStatement {
    /// Makes the statement on the server: the old identity key signs the
    /// new one.
    ///
    /// # Errors
    ///
    /// [`RotationError::SameKey`] if the new key is the old key.
    pub fn sign(old_key: &SigningKey, new_key: &PublicKey) -> Result<Self, RotationError> {
        let old_public = old_key.public_key();
        if old_public == *new_key {
            return Err(RotationError::SameKey);
        }
        let signature = old_key.sign(&rotation_message(&old_public, new_key));
        Ok(Self {
            old_key: old_public,
            new_key: *new_key,
            signature,
        })
    }

    /// Accepts a statement from its three parts if the signature is right.
    ///
    /// # Errors
    ///
    /// [`RotationError::SameKey`] or [`RotationError::Signature`].
    pub fn from_parts(
        old_key: PublicKey,
        new_key: PublicKey,
        signature: Signature,
    ) -> Result<Self, RotationError> {
        if old_key == new_key {
            return Err(RotationError::SameKey);
        }
        old_key.verify(&rotation_message(&old_key, &new_key), &signature)?;
        Ok(Self {
            old_key,
            new_key,
            signature,
        })
    }

    /// Reads and verifies an encoded statement:
    /// `old_key || new_key || signature`.
    ///
    /// # Errors
    ///
    /// [`RotationError::Length`] unless there are exactly 128 bytes,
    /// [`RotationError::Key`] if a key is not valid, otherwise as
    /// [`RotationStatement::from_parts`].
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, RotationError> {
        let bytes: &[u8; ROTATION_STATEMENT_LEN] = LengthError::check(bytes)?;
        let (old_key, rest) = bytes.split_at(PUBLIC_KEY_LEN);
        let (new_key, signature) = rest.split_at(PUBLIC_KEY_LEN);
        Self::from_parts(
            PublicKey::from_slice(old_key)?,
            PublicKey::from_slice(new_key)?,
            Signature::from_slice(signature)?,
        )
    }

    /// The encoded statement: `old_key || new_key || signature`.
    #[must_use]
    pub fn to_bytes(&self) -> [u8; ROTATION_STATEMENT_LEN] {
        encoding::concat(&[
            self.old_key.as_bytes(),
            self.new_key.as_bytes(),
            self.signature.as_bytes(),
        ])
    }

    /// Reads and verifies a list of statements encoded one after the other:
    /// a server's rotation history, from its file or from a peer.
    ///
    /// The length and the number of statements are checked first, so a
    /// hostile list costs at most [`MAX_ROTATION_STATEMENTS`] signature
    /// checks.
    ///
    /// # Errors
    ///
    /// [`RotationError::ListLength`] unless the length is a multiple of 128
    /// bytes, [`RotationError::TooMany`] for more than
    /// [`MAX_ROTATION_STATEMENTS`] statements, otherwise the error of the
    /// first statement that [`RotationStatement::from_bytes`] refuses.
    pub fn list_from_bytes(bytes: &[u8]) -> Result<Vec<Self>, RotationError> {
        if !bytes.len().is_multiple_of(ROTATION_STATEMENT_LEN) {
            return Err(RotationError::ListLength { found: bytes.len() });
        }
        let count = bytes.len() / ROTATION_STATEMENT_LEN;
        if count > MAX_ROTATION_STATEMENTS {
            return Err(RotationError::TooMany { found: count });
        }
        bytes
            .chunks_exact(ROTATION_STATEMENT_LEN)
            .map(Self::from_bytes)
            .collect()
    }

    /// Encodes a list of statements one after the other.
    #[must_use]
    pub fn list_to_bytes(statements: &[Self]) -> Vec<u8> {
        statements
            .iter()
            .flat_map(|statement| statement.to_bytes())
            .collect()
    }

    /// The key that signed: the one clients have pinned.
    #[must_use]
    pub const fn old_key(&self) -> &PublicKey {
        &self.old_key
    }

    /// The key that replaces it.
    #[must_use]
    pub const fn new_key(&self) -> &PublicKey {
        &self.new_key
    }

    /// The old key's signature.
    #[must_use]
    pub const fn signature(&self) -> &Signature {
        &self.signature
    }
}

/// What a client does with the identity key a server presented.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[must_use]
pub enum PinCheck {
    /// The key is the pinned key. Continue.
    Match,
    /// There is no pin. Finish the handshake, send nothing, show the
    /// server's six words and let the user trust or cancel. Nothing is
    /// trusted silently.
    Unknown,
    /// The key differs from the pin, and rotation statements lead from the
    /// pin to it. Replace the pin with the presented key and continue.
    Rotated,
    /// The key differs from the pin and nothing vouches for it. Stop: this
    /// is a hard failure, with no "accept anyway" on the connect path.
    Mismatch,
}

/// Compares the key a server presented with the pin on record.
///
/// `pin` is the key from the join link or from the server's record in the
/// client, `presented` the key in the server's certificate or `HelloAck`,
/// and `rotations` the statements the server sent (often none). The
/// presented key is accepted when a chain of statements leads from the pin
/// to it. Statements that are not part of such a chain are ignored and the
/// order does not matter, so a server may send its whole history to every
/// client. A chain of more than [`MAX_ROTATION_CHAIN`] statements is not
/// followed; a client that far behind has to be pinned again.
///
/// Proving that the peer holds `presented` is a separate step
/// ([`crate::handshake::verify_server_proof`]).
pub fn check_pin(
    pin: Option<&PublicKey>,
    presented: &PublicKey,
    rotations: &[RotationStatement],
) -> PinCheck {
    let Some(pin) = pin else {
        return PinCheck::Unknown;
    };
    if pin == presented {
        return PinCheck::Match;
    }

    // The keys the pin vouches for, in the order they are reached. Each
    // round follows one more statement, starting from the keys the round
    // before added, so round `n` reaches exactly the keys `n` statements
    // away from the pin, whatever the order of the list.
    let mut vouched: Vec<&PublicKey> = vec![pin];
    let mut round_start = 0;
    for _ in 0..MAX_ROTATION_CHAIN {
        let round_end = vouched.len();
        for statement in rotations {
            let continues_chain = vouched[round_start..round_end].contains(&&statement.old_key);
            if continues_chain && !vouched.contains(&&statement.new_key) {
                if statement.new_key == *presented {
                    return PinCheck::Rotated;
                }
                vouched.push(&statement.new_key);
            }
        }
        if vouched.len() == round_end {
            break;
        }
        round_start = round_end;
    }
    PinCheck::Mismatch
}
