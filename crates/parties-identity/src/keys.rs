//! Ed25519 keys and signatures.
//!
//! A user identity and a server identity are both an Ed25519 key pair
//! (`docs/rust-rewrite/04-connection-security.md` sections 4 and 5). The
//! types here replace `SecretKey`, `PublicKey` and `Signature` of
//! `common/include/parties/types.h` and the `ed25519_*` functions of
//! `common/src/crypto.cpp`.
//!
//! | Type | Holds | Secret |
//! |---|---|---|
//! | [`SigningKey`] | The 32-byte Ed25519 seed (RFC 8032 "private key") | Yes: wiped on drop, no `Debug`, no `Display`, no `Clone` |
//! | [`PublicKey`] | The 32-byte public key, checked to be a usable curve point | No |
//! | [`Signature`] | 64 bytes | No |
//!
//! # Verification is strict
//!
//! [`PublicKey::verify`] accepts exactly the signatures RFC 8032 signing
//! produces: a non-canonical `R` or `S`, a small-order `R` and a small-order
//! key are refused. Together with the checks in [`PublicKey::from_bytes`]
//! this means one key and one message have one valid signature form, and a
//! public key has one byte representation.

use core::fmt;
use core::str::FromStr;

use base64::Engine as _;
use base64::display::Base64Display;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ed25519_dalek::Signer as _;
use zeroize::{ZeroizeOnDrop, Zeroizing};

use crate::encoding::{self, HexError, LengthError};
use crate::fingerprint::Fingerprint;

/// Length of a public key in bytes.
pub const PUBLIC_KEY_LEN: usize = 32;

/// Length of the seed a signing key is made from, in bytes.
pub const SEED_LEN: usize = 32;

/// Length of a signature in bytes.
pub const SIGNATURE_LEN: usize = 64;

/// Length of the text form of a public key: unpadded base64url of 32 bytes.
pub const PUBLIC_KEY_TEXT_LEN: usize = 43;

/// Why bytes or text were not accepted as a key.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum KeyError {
    /// Wrong number of bytes, or of characters for a text form.
    #[error("key: {0}")]
    Length(#[from] LengthError),
    /// The text is not valid hex or canonical unpadded base64url.
    #[error("the key is not validly encoded")]
    Encoding,
    /// The bytes are not the canonical encoding of a point of the curve.
    #[error("the key is not a point of the curve")]
    InvalidPoint,
    /// The point has small order. Signatures by such a key prove nothing.
    #[error("the key has small order")]
    WeakKey,
}

impl From<HexError> for KeyError {
    fn from(error: HexError) -> Self {
        match error {
            HexError::Length(length) => Self::Length(length),
            HexError::Digit => Self::Encoding,
        }
    }
}

/// A signature did not verify.
///
/// The error carries no detail on purpose: a verifier never tells a peer
/// which check failed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, thiserror::Error)]
#[error("the signature does not verify")]
pub struct InvalidSignature;

/// The secret half of an identity: an Ed25519 signing key.
///
/// The key is wiped from memory when the value is dropped. It cannot be
/// printed (no `Debug`, no `Display`) and cannot be cloned: a second copy of
/// a signing key is made deliberately, through [`SigningKey::to_seed`], or
/// not at all.
///
/// ```
/// use parties_identity::SigningKey;
///
/// let key = SigningKey::from_seed(&[7; 32]);
/// let signature = key.sign(b"a message");
/// assert!(key.public_key().verify(b"a message", &signature).is_ok());
/// assert!(key.public_key().verify(b"another message", &signature).is_err());
/// ```
pub struct SigningKey {
    /// Wipes its seed on drop (`zeroize` feature of `ed25519-dalek`).
    inner: ed25519_dalek::SigningKey,
}

impl SigningKey {
    /// Makes the signing key of a 32-byte seed.
    ///
    /// The seed is the Ed25519 private key of RFC 8032 section 5.1.5, the
    /// same value the C++ client stores as `SecretKey`. Every seed is valid.
    #[must_use]
    pub fn from_seed(seed: &[u8; SEED_LEN]) -> Self {
        Self {
            inner: ed25519_dalek::SigningKey::from_bytes(seed),
        }
    }

    /// Reads a seed written as 64 hex digits, either case.
    ///
    /// This is the "private key" text the 1.x client exports and imports
    /// (`secret_key_to_hex`, `secret_key_from_hex`).
    ///
    /// # Errors
    ///
    /// [`KeyError::Length`] unless the text has exactly 64 characters,
    /// [`KeyError::Encoding`] if one of them is not a hex digit.
    pub fn from_seed_hex(text: &str) -> Result<Self, KeyError> {
        let mut seed = Zeroizing::new([0u8; SEED_LEN]);
        encoding::decode_hex(text, seed.as_mut_slice())?;
        Ok(Self::from_seed(&seed))
    }

    /// Creates a new random signing key from the operating system's
    /// random-number generator. This is how a server makes its identity key.
    ///
    /// # Panics
    ///
    /// If the operating system cannot provide random bytes. There is no
    /// sound way to continue without them
    /// (`docs/rust-rewrite/04-connection-security.md`, finding F21).
    #[cfg(feature = "os-rng")]
    #[must_use]
    pub fn generate() -> Self {
        let mut seed = Zeroizing::new([0u8; SEED_LEN]);
        crate::fill_random(seed.as_mut_slice());
        Self::from_seed(&seed)
    }

    /// A copy of the seed, for the OS keystore or the server's key file.
    ///
    /// The copy is wiped when dropped. This is the only way to get secret
    /// bytes out of a signing key.
    #[must_use]
    pub fn to_seed(&self) -> Zeroizing<[u8; SEED_LEN]> {
        Zeroizing::new(self.inner.to_bytes())
    }

    /// The seed as 64 lowercase hex digits: the 1.x "private key" export.
    #[must_use]
    pub fn to_seed_hex(&self) -> Zeroizing<String> {
        let seed = self.to_seed();
        let mut text = Zeroizing::new(String::with_capacity(SEED_LEN * 2));
        for &byte in seed.iter() {
            text.extend(encoding::hex_pair(byte));
        }
        text
    }

    /// The public key that verifies this key's signatures.
    #[must_use]
    pub fn public_key(&self) -> PublicKey {
        // A key made from a seed is a multiple of eight times the base
        // point, so it is canonical and never of small order.
        PublicKey(self.inner.verifying_key().to_bytes())
    }

    /// The fingerprint of the public key.
    #[must_use]
    pub fn fingerprint(&self) -> Fingerprint {
        self.public_key().fingerprint()
    }

    /// Signs a message (RFC 8032, pure Ed25519). The same key and message
    /// always give the same signature.
    ///
    /// The statements of the protocol are built and signed by the other
    /// modules; use this directly only for a message with its own domain
    /// label.
    #[must_use]
    pub fn sign(&self, message: &[u8]) -> Signature {
        Signature(self.inner.sign(message).to_bytes())
    }
}

/// The seed is wiped by the wrapped key when it is dropped.
impl ZeroizeOnDrop for SigningKey {}

// That wipe comes from the `zeroize` feature of `ed25519-dalek`. Without the
// feature this does not compile, instead of quietly leaving seeds in memory.
const _: fn() = || {
    fn wipes_on_drop<T: ZeroizeOnDrop>() {}
    wipes_on_drop::<ed25519_dalek::SigningKey>();
};

/// The public half of an identity: an Ed25519 public key.
///
/// A value of this type is always the canonical encoding of a curve point
/// that is not of small order, so it can be stored, compared and hashed as
/// plain bytes.
///
/// The text form (`Display`, `FromStr`) is unpadded base64url (RFC 4648
/// section 5), 43 characters. It is what join links and configuration files
/// carry.
///
/// ```
/// use parties_identity::{PublicKey, SigningKey};
///
/// let key = SigningKey::from_seed(&[7; 32]).public_key();
/// let text = key.to_string();
/// assert_eq!(text, "6kpsY-KcUgq-9VB7Ey7F-ZVHdq6-vnuSQh7qaRRG0iw");
/// assert_eq!(text.parse::<PublicKey>(), Ok(key));
/// ```
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PublicKey([u8; PUBLIC_KEY_LEN]);

impl PublicKey {
    /// Accepts 32 bytes as a public key.
    ///
    /// # Errors
    ///
    /// [`KeyError::InvalidPoint`] if the bytes are not the canonical
    /// encoding of a curve point, [`KeyError::WeakKey`] if the point has
    /// small order.
    pub fn from_bytes(bytes: &[u8; PUBLIC_KEY_LEN]) -> Result<Self, KeyError> {
        let key =
            ed25519_dalek::VerifyingKey::from_bytes(bytes).map_err(|_| KeyError::InvalidPoint)?;
        // Decompression follows ZIP-215 and accepts a few non-canonical
        // encodings. Refuse them, so that a key has one representation.
        if key.to_edwards().compress().to_bytes() != *bytes {
            return Err(KeyError::InvalidPoint);
        }
        if key.is_weak() {
            return Err(KeyError::WeakKey);
        }
        Ok(Self(*bytes))
    }

    /// Accepts a byte slice as a public key.
    ///
    /// # Errors
    ///
    /// [`KeyError::Length`] unless the slice has 32 bytes, otherwise as
    /// [`PublicKey::from_bytes`].
    pub fn from_slice(bytes: &[u8]) -> Result<Self, KeyError> {
        Self::from_bytes(LengthError::check(bytes)?)
    }

    /// The 32 bytes of the key.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; PUBLIC_KEY_LEN] {
        &self.0
    }

    /// The 32 bytes of the key.
    #[must_use]
    pub const fn to_bytes(self) -> [u8; PUBLIC_KEY_LEN] {
        self.0
    }

    /// The fingerprint of the key: its SHA-256.
    #[must_use]
    pub fn fingerprint(&self) -> Fingerprint {
        Fingerprint::of(self)
    }

    /// Checks a signature over a message.
    ///
    /// # Errors
    ///
    /// [`InvalidSignature`] unless the signature is the one this key makes
    /// for this message (see the module documentation on strictness).
    pub fn verify(&self, message: &[u8], signature: &Signature) -> Result<(), InvalidSignature> {
        let key = ed25519_dalek::VerifyingKey::from_bytes(&self.0).map_err(|_| InvalidSignature)?;
        let signature = ed25519_dalek::Signature::from_bytes(&signature.0);
        key.verify_strict(message, &signature)
            .map_err(|_| InvalidSignature)
    }
}

impl fmt::Display for PublicKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&Base64Display::new(&self.0, &URL_SAFE_NO_PAD), f)
    }
}

impl fmt::Debug for PublicKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PublicKey({self})")
    }
}

impl FromStr for PublicKey {
    type Err = KeyError;

    /// Reads the 43-character unpadded base64url form.
    ///
    /// Padding, whitespace, the standard alphabet (`+`, `/`) and unused bits
    /// that are not zero are all refused, so a key has one text form.
    fn from_str(text: &str) -> Result<Self, KeyError> {
        if text.len() != PUBLIC_KEY_TEXT_LEN {
            return Err(KeyError::Length(LengthError {
                expected: PUBLIC_KEY_TEXT_LEN,
                found: text.len(),
            }));
        }
        // Room for the decoder's estimate, which rounds up to whole groups.
        let mut buffer = [0u8; PUBLIC_KEY_LEN + 4];
        let written = URL_SAFE_NO_PAD
            .decode_slice(text, &mut buffer)
            .map_err(|_| KeyError::Encoding)?;
        let bytes = buffer
            .get(..written)
            .and_then(|decoded| <&[u8; PUBLIC_KEY_LEN]>::try_from(decoded).ok())
            .ok_or(KeyError::Encoding)?;
        Self::from_bytes(bytes)
    }
}

/// An Ed25519 signature: 64 bytes.
///
/// Any 64 bytes can be held; whether they are a valid signature is decided
/// by [`PublicKey::verify`].
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Signature([u8; SIGNATURE_LEN]);

impl Signature {
    /// Wraps 64 bytes.
    #[must_use]
    pub const fn from_bytes(bytes: [u8; SIGNATURE_LEN]) -> Self {
        Self(bytes)
    }

    /// Wraps a byte slice.
    ///
    /// # Errors
    ///
    /// [`LengthError`] unless the slice has 64 bytes.
    pub fn from_slice(bytes: &[u8]) -> Result<Self, LengthError> {
        LengthError::check(bytes).map(|bytes| Self(*bytes))
    }

    /// The 64 bytes of the signature.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; SIGNATURE_LEN] {
        &self.0
    }

    /// The 64 bytes of the signature.
    #[must_use]
    pub const fn to_bytes(self) -> [u8; SIGNATURE_LEN] {
        self.0
    }
}

impl fmt::Debug for Signature {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Signature(")?;
        encoding::write_hex(f, &self.0)?;
        f.write_str(")")
    }
}
