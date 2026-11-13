//! Linking a second key to an account
//! (`docs/rust-rewrite/13-profiles-access-and-hardware-keys.md` sections 6.4
//! and 7.4).
//!
//! An account on a server can hold several keys: a spare, a key born from a
//! passkey, a recovery phrase kept on paper. `LinkKey` adds one. It is sent
//! on a connection that is signed in with a key the account already has, and
//! it carries a proof made with the new key, so that nobody can attach a key
//! they do not hold.
//!
//! Both keys speak about the same statement:
//!
//! ```text
//! statement = "parties-link-key-v1" || 0x00 || exporter || server_identity
//!             || account_key || new_key
//!
//! proof     = Sign(new_key, statement)        -- carried by LinkKey, required
//! approval  = Sign(account_key, statement)    -- optional
//! ```
//!
//! `exporter` is the TLS exporter of the connection `LinkKey` is sent on
//! ([`crate::handshake::Exporter`]), `server_identity` the server's identity
//! key, `account_key` the key that connection signed in with, and `new_key`
//! the key being added; the keys are 32-byte Ed25519 public keys.
//!
//! What the fields are for:
//!
//! | Field | Stops |
//! |---|---|
//! | `exporter` | Replaying the proof on any other connection |
//! | `server_identity` | A server relaying the proof to another server |
//! | `account_key` | Attaching the new key to an account its holder did not mean |
//! | `new_key` | Using the proof for a different key |
//!
//! The server needs only the proof: the connection is already signed in as
//! the account, which is the account's consent. The approval is the same
//! statement signed by the account's existing key. Nothing in protocol 2.0
//! requires it; it exists so that a link can be shown to a third party
//! later without trusting the server's word.
//!
//! This version covers Ed25519 keys on both sides. Keys on a hardware
//! authenticator sign a WebAuthn assertion instead of raw bytes and get
//! their own statement when that work is done.

use crate::encoding;
use crate::handshake::{EXPORTER_LEN, Exporter};
use crate::keys::{InvalidSignature, PUBLIC_KEY_LEN, PublicKey, Signature, SigningKey};

/// Domain label of the key-link statement.
pub const LINK_KEY_LABEL: &[u8] = b"parties-link-key-v1";

/// Length of the key-link statement in bytes.
pub const LINK_KEY_MESSAGE_LEN: usize =
    LINK_KEY_LABEL.len() + 1 + EXPORTER_LEN + 3 * PUBLIC_KEY_LEN;

/// Why a key link was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum LinkKeyError {
    /// The new key is the key the account already signed in with.
    #[error("a key cannot be linked to itself")]
    SameKey,
    /// The signing key is not the key the statement names for that role.
    #[error("the signing key is not the key named in the statement")]
    WrongKey,
    /// The signature is not that key's signature over this statement.
    #[error("key link: {0}")]
    Signature(#[from] InvalidSignature),
}

/// The statement that links a new key to an account, on one connection to
/// one server.
///
/// ```
/// use parties_identity::handshake::Exporter;
/// use parties_identity::{LinkStatement, SigningKey};
///
/// let server = SigningKey::from_seed(&[1; 32]).public_key();
/// let account_key = SigningKey::from_seed(&[2; 32]);
/// let new_key = SigningKey::from_seed(&[3; 32]);
/// let exporter = Exporter::from_bytes([4; 32]);
///
/// // Client: signed in with `account_key`, it proves that it holds `new_key`.
/// let statement = LinkStatement::new(
///     &exporter,
///     &server,
///     &account_key.public_key(),
///     &new_key.public_key(),
/// )?;
/// let proof = statement.sign_with_new_key(&new_key)?;
///
/// // Server: builds the same statement from what it knows about the
/// // connection and checks the proof before it stores the key.
/// statement.verify_new_key(&proof)?;
/// # Ok::<(), parties_identity::LinkKeyError>(())
/// ```
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LinkStatement {
    exporter: Exporter,
    server_identity: PublicKey,
    account_key: PublicKey,
    new_key: PublicKey,
}

impl LinkStatement {
    /// Describes one link.
    ///
    /// `account_key` is the key the connection is signed in with, `new_key`
    /// the key to add.
    ///
    /// # Errors
    ///
    /// [`LinkKeyError::SameKey`] if the two keys are the same.
    pub fn new(
        exporter: &Exporter,
        server_identity: &PublicKey,
        account_key: &PublicKey,
        new_key: &PublicKey,
    ) -> Result<Self, LinkKeyError> {
        if account_key == new_key {
            return Err(LinkKeyError::SameKey);
        }
        Ok(Self {
            exporter: *exporter,
            server_identity: *server_identity,
            account_key: *account_key,
            new_key: *new_key,
        })
    }

    /// The bytes both keys sign: `"parties-link-key-v1" || 0x00 || exporter ||
    /// server_identity || account_key || new_key`.
    #[must_use]
    pub fn message(&self) -> [u8; LINK_KEY_MESSAGE_LEN] {
        encoding::concat(&[
            LINK_KEY_LABEL,
            &[0],
            self.exporter.as_bytes(),
            self.server_identity.as_bytes(),
            self.account_key.as_bytes(),
            self.new_key.as_bytes(),
        ])
    }

    /// The key the connection is signed in with.
    #[must_use]
    pub const fn account_key(&self) -> &PublicKey {
        &self.account_key
    }

    /// The key being added.
    #[must_use]
    pub const fn new_key(&self) -> &PublicKey {
        &self.new_key
    }

    /// Makes the proof `LinkKey` carries: the new key signs the statement.
    ///
    /// # Errors
    ///
    /// [`LinkKeyError::WrongKey`] if `new_key` is not the statement's new
    /// key.
    pub fn sign_with_new_key(&self, new_key: &SigningKey) -> Result<Signature, LinkKeyError> {
        self.sign_as(&self.new_key, new_key)
    }

    /// Checks the proof `LinkKey` carries, on the server.
    ///
    /// # Errors
    ///
    /// [`LinkKeyError::Signature`] unless the new key signed exactly this
    /// statement.
    pub fn verify_new_key(&self, proof: &Signature) -> Result<(), LinkKeyError> {
        Ok(self.new_key.verify(&self.message(), proof)?)
    }

    /// Makes the optional approval: the account's existing key signs the
    /// same statement.
    ///
    /// # Errors
    ///
    /// [`LinkKeyError::WrongKey`] if `account_key` is not the statement's
    /// account key.
    pub fn sign_with_account_key(
        &self,
        account_key: &SigningKey,
    ) -> Result<Signature, LinkKeyError> {
        self.sign_as(&self.account_key, account_key)
    }

    /// Checks the optional approval.
    ///
    /// # Errors
    ///
    /// [`LinkKeyError::Signature`] unless the account key signed exactly
    /// this statement.
    pub fn verify_account_key(&self, approval: &Signature) -> Result<(), LinkKeyError> {
        Ok(self.account_key.verify(&self.message(), approval)?)
    }

    fn sign_as(&self, named: &PublicKey, key: &SigningKey) -> Result<Signature, LinkKeyError> {
        if key.public_key() != *named {
            return Err(LinkKeyError::WrongKey);
        }
        Ok(key.sign(&self.message()))
    }
}
