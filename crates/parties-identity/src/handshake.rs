//! The proofs of the 2.0 handshake
//! (`docs/rust-rewrite/03-protocol-v2.md` section 4).
//!
//! Every proof covers the TLS exporter of the connection it is made on, so
//! nothing signed for one connection is valid on another, on any server
//! (finding F1). There is no timestamp and no replay cache.
//!
//! ```text
//! exporter       = TLS-Exporter("EXPORTER-parties-auth-v2", context = "", 32 bytes)
//!
//! server_proof   = Sign(server_identity_key,
//!                       "parties-server-proof-v2" || 0x00 || exporter)
//!
//! signature      = Sign(user_key,
//!                       "parties-auth-v2" || 0x00 || exporter || server_identity || user_key)
//!
//! password_proof = HMAC-SHA256(K, "parties-join-pw-v2" || 0x00 || exporter)
//!         where K = Argon2id(password, salt, parameters from HelloAck), 32 bytes
//! ```
//!
//! `server_identity` and `user_key` are the 32-byte Ed25519 public keys. The
//! labels are ASCII without a terminator; the single zero byte separates the
//! label from the fields. Signatures are pure Ed25519 (RFC 8032) and are
//! verified strictly ([`crate::keys`]).
//!
//! The labels live here, not in `parties-proto`: this crate is the one place
//! that builds and checks these messages, for the server and the client.
//!
//! # The password key
//!
//! `K` is Argon2id version 1.3 with a 32-byte output and no secret or
//! associated data. The server stores `K` and the parameters, never the
//! password; the password never crosses the wire. Because the parameters
//! reach the client from a server it may not trust yet, they are bounded on
//! both sides ([`PasswordKdfParams`]): a floor, so that a man in the middle
//! cannot ask for a proof that is cheap to attack offline, and a ceiling, so
//! that a hostile server cannot make the client burn memory and time.
//!
//! # Example
//!
//! ```
//! use parties_identity::handshake::{
//!     Exporter, sign_server_proof, sign_user_auth, verify_server_proof, verify_user_auth,
//! };
//! use parties_identity::SigningKey;
//!
//! let server_key = SigningKey::from_seed(&[1; 32]);
//! let user_key = SigningKey::from_seed(&[2; 32]);
//! // Both sides take this from their TLS stack.
//! let exporter = Exporter::from_bytes([3; 32]);
//!
//! // Server, in HelloAck.
//! let proof = sign_server_proof(&server_key, &exporter);
//! // Client: the key it pinned must have signed this connection.
//! verify_server_proof(&server_key.public_key(), &exporter, &proof)?;
//!
//! // Client, in Authenticate.
//! let signature = sign_user_auth(&user_key, &exporter, &server_key.public_key());
//! // Server.
//! verify_user_auth(&user_key.public_key(), &exporter, &server_key.public_key(), &signature)?;
//!
//! // The same signature on another connection is worthless.
//! let other = Exporter::from_bytes([4; 32]);
//! assert!(
//!     verify_user_auth(&user_key.public_key(), &other, &server_key.public_key(), &signature)
//!         .is_err()
//! );
//! # Ok::<(), parties_identity::InvalidSignature>(())
//! ```

use core::fmt;

use hmac::{Hmac, KeyInit, Mac};
use sha2::Sha256;
use zeroize::{ZeroizeOnDrop, Zeroizing};

use crate::encoding::{self, LengthError};
use crate::keys::{InvalidSignature, PUBLIC_KEY_LEN, PublicKey, Signature, SigningKey};

/// Label of the TLS exporter both sides derive (RFC 8446 section 7.5).
pub const EXPORTER_LABEL: &[u8] = b"EXPORTER-parties-auth-v2";

/// Context of the TLS exporter: empty. In TLS 1.3 an empty context and an
/// absent one give the same value.
pub const EXPORTER_CONTEXT: &[u8] = b"";

/// Length of the exporter value in bytes.
pub const EXPORTER_LEN: usize = 32;

/// Domain label of `server_proof`.
pub const SERVER_PROOF_LABEL: &[u8] = b"parties-server-proof-v2";

/// Domain label of the user's `signature` in `Authenticate`.
pub const USER_AUTH_LABEL: &[u8] = b"parties-auth-v2";

/// Domain label of `password_proof`.
pub const JOIN_PASSWORD_LABEL: &[u8] = b"parties-join-pw-v2";

/// Length of the message `server_proof` signs.
pub const SERVER_PROOF_MESSAGE_LEN: usize = SERVER_PROOF_LABEL.len() + 1 + EXPORTER_LEN;

/// Length of the message the user's `signature` signs.
pub const USER_AUTH_MESSAGE_LEN: usize =
    USER_AUTH_LABEL.len() + 1 + EXPORTER_LEN + 2 * PUBLIC_KEY_LEN;

/// Length of the password key `K` in bytes.
pub const PASSWORD_KEY_LEN: usize = 32;

/// Length of `password_proof` in bytes.
pub const PASSWORD_PROOF_LEN: usize = 32;

/// The TLS exporter value of one connection: the channel binding.
///
/// Both sides obtain it from their TLS stack with [`EXPORTER_LABEL`],
/// [`EXPORTER_CONTEXT`] and [`EXPORTER_LEN`]. It is not a long-term secret,
/// but there is no reason to log it, so `Debug` does not print it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Exporter([u8; EXPORTER_LEN]);

impl Exporter {
    /// Wraps the 32 bytes the TLS stack exported.
    #[must_use]
    pub const fn from_bytes(bytes: [u8; EXPORTER_LEN]) -> Self {
        Self(bytes)
    }

    /// Wraps a byte slice.
    ///
    /// # Errors
    ///
    /// [`LengthError`] unless the slice has 32 bytes.
    pub fn from_slice(bytes: &[u8]) -> Result<Self, LengthError> {
        LengthError::check(bytes).map(|bytes| Self(*bytes))
    }

    /// The 32 bytes of the exporter value.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; EXPORTER_LEN] {
        &self.0
    }
}

impl fmt::Debug for Exporter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Exporter(..)")
    }
}

/// The message `server_proof` signs:
/// `"parties-server-proof-v2" || 0x00 || exporter`.
#[must_use]
pub fn server_proof_message(exporter: &Exporter) -> [u8; SERVER_PROOF_MESSAGE_LEN] {
    encoding::concat(&[SERVER_PROOF_LABEL, &[0], exporter.as_bytes()])
}

/// Makes `server_proof` for `HelloAck`: the server's identity key signs this
/// connection.
#[must_use]
pub fn sign_server_proof(server_key: &SigningKey, exporter: &Exporter) -> Signature {
    server_key.sign(&server_proof_message(exporter))
}

/// Checks `server_proof`: the holder of `server_identity` is the peer of
/// this connection.
///
/// This says nothing about whether `server_identity` is the right server;
/// that is the pin's job ([`crate::trust::check_pin`]).
///
/// # Errors
///
/// [`InvalidSignature`] if the proof was made by another key or for another
/// connection.
pub fn verify_server_proof(
    server_identity: &PublicKey,
    exporter: &Exporter,
    proof: &Signature,
) -> Result<(), InvalidSignature> {
    server_identity.verify(&server_proof_message(exporter), proof)
}

/// The message the user's `signature` signs:
/// `"parties-auth-v2" || 0x00 || exporter || server_identity || user_key`.
#[must_use]
pub fn user_auth_message(
    exporter: &Exporter,
    server_identity: &PublicKey,
    user_key: &PublicKey,
) -> [u8; USER_AUTH_MESSAGE_LEN] {
    encoding::concat(&[
        USER_AUTH_LABEL,
        &[0],
        exporter.as_bytes(),
        server_identity.as_bytes(),
        user_key.as_bytes(),
    ])
}

/// Makes the `signature` of `Authenticate`: the user's key signs this
/// connection to this server.
///
/// `server_identity` must be the key the client decided to trust for this
/// connection, never a key taken from the same message it is about to
/// answer without the pin check.
#[must_use]
pub fn sign_user_auth(
    user_key: &SigningKey,
    exporter: &Exporter,
    server_identity: &PublicKey,
) -> Signature {
    let message = user_auth_message(exporter, server_identity, &user_key.public_key());
    user_key.sign(&message)
}

/// Checks the `signature` of `Authenticate` on the server.
///
/// `server_identity` is the server's own identity key and `exporter` the
/// value of the connection the message arrived on.
///
/// # Errors
///
/// [`InvalidSignature`] if the signature was made by another key, for
/// another server or for another connection.
pub fn verify_user_auth(
    user_key: &PublicKey,
    exporter: &Exporter,
    server_identity: &PublicKey,
    signature: &Signature,
) -> Result<(), InvalidSignature> {
    user_key.verify(
        &user_auth_message(exporter, server_identity, user_key),
        signature,
    )
}

/// Why Argon2id parameters were refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum KdfParamsError {
    /// Memory outside [`PasswordKdfParams::MIN_MEMORY_KIB`] to
    /// [`PasswordKdfParams::MAX_MEMORY_KIB`].
    #[error("Argon2id memory of {found} KiB is outside the accepted range")]
    Memory {
        /// The memory that was asked for, in KiB.
        found: u32,
    },
    /// Passes outside [`PasswordKdfParams::MIN_ITERATIONS`] to
    /// [`PasswordKdfParams::MAX_ITERATIONS`].
    #[error("{found} Argon2id passes are outside the accepted range")]
    Iterations {
        /// The number of passes that was asked for.
        found: u32,
    },
    /// Lanes outside 1 to [`PasswordKdfParams::MAX_PARALLELISM`].
    #[error("an Argon2id parallelism of {found} is outside the accepted range")]
    Parallelism {
        /// The parallelism that was asked for.
        found: u32,
    },
    /// Salt outside [`PasswordKdfParams::MIN_SALT_LEN`] to
    /// [`PasswordKdfParams::MAX_SALT_LEN`] bytes.
    #[error("an Argon2id salt of {found} bytes is outside the accepted range")]
    SaltLength {
        /// The length of the salt that was given, in bytes.
        found: usize,
    },
}

/// The Argon2id parameters and salt of a server's join password, as
/// `HelloAck` carries them.
///
/// A value of this type is always within the bounds below, so deriving a key
/// with it is neither trivially cheap nor a way to exhaust the client.
///
/// | Bound | Value | Reason |
/// |---|---|---|
/// | Memory | 19 MiB to 256 MiB | Floor: the weakest setting OWASP lists for Argon2id (19 MiB with two passes). Ceiling: what a phone can spare |
/// | Passes | 2 to 16 | |
/// | Parallelism | 1 to 16 | |
/// | Salt | 16 to 64 bytes | RFC 9106 recommends 16 |
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PasswordKdfParams {
    memory_kib: u32,
    iterations: u32,
    parallelism: u32,
    salt: Vec<u8>,
}

impl PasswordKdfParams {
    /// Least memory, in KiB.
    pub const MIN_MEMORY_KIB: u32 = 19 * 1024;
    /// Most memory, in KiB.
    pub const MAX_MEMORY_KIB: u32 = 256 * 1024;
    /// Fewest passes.
    pub const MIN_ITERATIONS: u32 = 2;
    /// Most passes.
    pub const MAX_ITERATIONS: u32 = 16;
    /// Most lanes.
    pub const MAX_PARALLELISM: u32 = 16;
    /// Shortest salt, in bytes.
    pub const MIN_SALT_LEN: usize = 16;
    /// Longest salt, in bytes.
    pub const MAX_SALT_LEN: usize = 64;

    /// Memory a server should choose, in KiB: 64 MiB. With
    /// [`Self::RECOMMENDED_ITERATIONS`] and
    /// [`Self::RECOMMENDED_PARALLELISM`] this is the second recommended
    /// parameter set of RFC 9106 section 4.
    pub const RECOMMENDED_MEMORY_KIB: u32 = 64 * 1024;
    /// Passes a server should choose.
    pub const RECOMMENDED_ITERATIONS: u32 = 3;
    /// Lanes a server should choose.
    pub const RECOMMENDED_PARALLELISM: u32 = 4;

    /// Accepts parameters that are within the bounds.
    ///
    /// # Errors
    ///
    /// A [`KdfParamsError`] naming the first value that is out of range.
    pub fn new(
        memory_kib: u32,
        iterations: u32,
        parallelism: u32,
        salt: &[u8],
    ) -> Result<Self, KdfParamsError> {
        if !(Self::MIN_MEMORY_KIB..=Self::MAX_MEMORY_KIB).contains(&memory_kib) {
            return Err(KdfParamsError::Memory { found: memory_kib });
        }
        if !(Self::MIN_ITERATIONS..=Self::MAX_ITERATIONS).contains(&iterations) {
            return Err(KdfParamsError::Iterations { found: iterations });
        }
        if !(1..=Self::MAX_PARALLELISM).contains(&parallelism) {
            return Err(KdfParamsError::Parallelism { found: parallelism });
        }
        if !(Self::MIN_SALT_LEN..=Self::MAX_SALT_LEN).contains(&salt.len()) {
            return Err(KdfParamsError::SaltLength { found: salt.len() });
        }
        Ok(Self {
            memory_kib,
            iterations,
            parallelism,
            salt: salt.to_vec(),
        })
    }

    /// The recommended parameters with the given salt. The salt is public
    /// and must be random, one per password.
    ///
    /// # Errors
    ///
    /// [`KdfParamsError::SaltLength`] if the salt is too short or too long.
    pub fn recommended(salt: &[u8]) -> Result<Self, KdfParamsError> {
        Self::new(
            Self::RECOMMENDED_MEMORY_KIB,
            Self::RECOMMENDED_ITERATIONS,
            Self::RECOMMENDED_PARALLELISM,
            salt,
        )
    }

    /// Memory in KiB.
    #[must_use]
    pub const fn memory_kib(&self) -> u32 {
        self.memory_kib
    }

    /// Number of passes.
    #[must_use]
    pub const fn iterations(&self) -> u32 {
        self.iterations
    }

    /// Number of lanes.
    #[must_use]
    pub const fn parallelism(&self) -> u32 {
        self.parallelism
    }

    /// The salt.
    #[must_use]
    pub fn salt(&self) -> &[u8] {
        &self.salt
    }
}

/// Deriving the password key failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum KdfError {
    /// The memory the parameters ask for could not be allocated.
    #[error("not enough memory for Argon2id")]
    OutOfMemory,
    /// Argon2id refused the input. Parameters built by
    /// [`PasswordKdfParams::new`] do not cause this.
    #[error("Argon2id refused the parameters")]
    Rejected,
}

/// A `password_proof` did not verify.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, thiserror::Error)]
#[error("the password proof does not verify")]
pub struct InvalidPasswordProof;

/// The key behind `password_proof`: `K = Argon2id(password, salt,
/// parameters)`.
///
/// The server stores this value instead of the password. Whoever holds it
/// can make valid proofs for that server, so it is a secret: wiped on drop,
/// not printable, not cloneable.
pub struct PasswordKey(Zeroizing<[u8; PASSWORD_KEY_LEN]>);

impl PasswordKey {
    /// Derives the key from a password.
    ///
    /// The password's UTF-8 bytes are used as given; the caller applies the
    /// same Unicode normalisation (NFC) on every platform before calling.
    /// This takes the time and memory the parameters ask for, so it belongs
    /// on a blocking thread.
    ///
    /// # Errors
    ///
    /// [`KdfError::OutOfMemory`] if the memory cannot be allocated.
    pub fn derive(password: &str, params: &PasswordKdfParams) -> Result<Self, KdfError> {
        let argon_params = argon2::Params::new(
            params.memory_kib,
            params.iterations,
            params.parallelism,
            Some(PASSWORD_KEY_LEN),
        )
        .map_err(|_| KdfError::Rejected)?;
        let argon = argon2::Argon2::new(
            argon2::Algorithm::Argon2id,
            argon2::Version::V0x13,
            argon_params,
        );
        let mut key = Zeroizing::new([0u8; PASSWORD_KEY_LEN]);
        argon
            .hash_password_into(password.as_bytes(), &params.salt, key.as_mut_slice())
            .map_err(|error| match error {
                argon2::Error::OutOfMemory => KdfError::OutOfMemory,
                _ => KdfError::Rejected,
            })?;
        Ok(Self(key))
    }

    /// Wraps a key the server read from its database.
    #[must_use]
    pub fn from_bytes(bytes: &[u8; PASSWORD_KEY_LEN]) -> Self {
        Self(Zeroizing::new(*bytes))
    }

    /// The key bytes, for the server's database. This reveals the secret.
    #[must_use]
    pub fn expose(&self) -> &[u8; PASSWORD_KEY_LEN] {
        &self.0
    }

    /// The MAC over this connection's exporter.
    fn mac(&self, exporter: &Exporter) -> Hmac<Sha256> {
        let mut mac = <Hmac<Sha256> as KeyInit>::new_from_slice(self.0.as_slice())
            .expect("HMAC accepts a key of any length");
        mac.update(JOIN_PASSWORD_LABEL);
        mac.update(&[0]);
        mac.update(exporter.as_bytes());
        mac
    }

    /// Makes `password_proof` for `Authenticate`:
    /// `HMAC-SHA256(K, "parties-join-pw-v2" || 0x00 || exporter)`.
    #[must_use]
    pub fn proof(&self, exporter: &Exporter) -> PasswordProof {
        PasswordProof(self.mac(exporter).finalize().into_bytes().into())
    }

    /// Checks a `password_proof` on the server, in constant time.
    ///
    /// # Errors
    ///
    /// [`InvalidPasswordProof`] if the proof has the wrong length, was made
    /// from another password or for another connection.
    pub fn verify(&self, exporter: &Exporter, proof: &[u8]) -> Result<(), InvalidPasswordProof> {
        self.mac(exporter)
            .verify_slice(proof)
            .map_err(|_| InvalidPasswordProof)
    }
}

/// The key is wiped when dropped.
impl ZeroizeOnDrop for PasswordKey {}

/// A `password_proof`: 32 bytes, valid on one connection only.
///
/// `Debug` does not print it: together with the exporter it is what an
/// offline attack on the password needs. It has no `==` either: a server
/// compares proofs with [`PasswordKey::verify`], which takes the same time
/// whatever the proof.
#[derive(Clone, Copy)]
pub struct PasswordProof([u8; PASSWORD_PROOF_LEN]);

impl PasswordProof {
    /// The 32 bytes to put into `Authenticate`.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; PASSWORD_PROOF_LEN] {
        &self.0
    }
}

impl fmt::Debug for PasswordProof {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("PasswordProof(..)")
    }
}
