//! Cryptographic identity of Parties 2.0.
//!
//! One implementation of everything that makes or checks a proof of who
//! someone is, used by the server, the client and `partiesctl`. Everything
//! here is a pure function of its arguments: no network, no files, no clock,
//! no `unsafe`. The only system call is the operating system's random-number
//! generator, behind the `os-rng` feature and only in the functions named
//! `generate`.
//!
//! | Module | Document, section | What it holds |
//! |---|---|---|
//! | [`keys`] | 04, 4 and 5 | [`SigningKey`], [`PublicKey`], [`Signature`]; strict Ed25519 |
//! | [`fingerprint`] | 04, 4 | [`Fingerprint`] in colon-separated hex, and the [`SixWords`] of a server |
//! | [`phrase`] | 04, 5.2 | Recovery phrases: [`PhraseV1`] of the C++ client, [`PhraseV2`] (BIP-39), [`import_phrase`] |
//! | [`handshake`] | 03, 4 | The exporter label, `server_proof`, the user's `signature`, `password_proof` |
//! | [`join`] | 13, 5; 04, 4 | [`JoinCode`] and [`JoinLink`] |
//! | [`trust`] | 04, 4 | [`check_pin`] and [`RotationStatement`] |
//! | [`level`] | 04, 6.3 | The identity level: [`identity_level`], [`search_identity_level`] |
//! | [`passkey`] | 13, 7.2 | From a passkey's PRF output to the identity key |
//! | [`key_link`] | 13, 6.4 and 7.4 | [`LinkStatement`]: adding a second key to an account |
//! | [`wordlist`] | | The BIP-39 English word list |
//!
//! The documents are in `docs/rust-rewrite/`. Where they leave an encoding,
//! a label or a parameter open, the module that needs it defines it and says
//! so in its documentation.
//!
//! # Secrets
//!
//! Types that hold a secret ([`SigningKey`], [`PhraseV1`], [`PhraseV2`],
//! [`RecoveryPhrase`], [`handshake::PasswordKey`]) are wiped when dropped
//! and implement neither `Debug` nor `Display` nor `Clone`. Getting secret
//! bytes out takes a call whose name says so (`expose`, `to_seed`). Types
//! that are sensitive without being long-term secrets ([`JoinCode`],
//! [`JoinLink`], [`handshake::Exporter`], [`handshake::PasswordProof`])
//! have a `Debug` that prints no secret part. Errors never quote the input
//! they refuse.
//!
//! Wiping is best effort: it covers the values this crate owns, not copies
//! the compiler makes on the stack or copies the caller holds.
//!
//! # Not in this crate
//!
//! - Verifying FIDO2 or WebAuthn assertions (hardware keys, document 13
//!   section 6). Only Ed25519 software keys sign and verify here.
//! - The OS keystore, files and databases: `parties-store` and the server
//!   keep what this crate derives.
//! - Networking, and obtaining the TLS exporter: `parties-transport`.
//! - TLS certificates of servers.
//!
//! # Example
//!
//! ```
//! use parties_identity::handshake::{Exporter, sign_user_auth, verify_user_auth};
//! use parties_identity::{JoinLink, PhraseV2, PinCheck, SigningKey, check_pin};
//!
//! // A server has an identity key and hands out a join link that carries it.
//! let server_key = SigningKey::from_seed(&[1; 32]);
//! let server = server_key.public_key();
//! let link = JoinLink::parse(&format!("parties://example.org:7800/j/7KQ2M-9X01T#k={server}"))?;
//!
//! // A user's identity comes from a recovery phrase.
//! let phrase = PhraseV2::from_entropy(&[0x5a; 16])?;
//! let user_key = phrase.signing_key();
//!
//! // On connecting, the key the server presents must be the pinned one ...
//! assert_eq!(check_pin(Some(link.server_key()), &server, &[]), PinCheck::Match);
//!
//! // ... and the login is a signature bound to this connection and server.
//! let exporter = Exporter::from_bytes([9; 32]);
//! let signature = sign_user_auth(&user_key, &exporter, &server);
//! verify_user_auth(&user_key.public_key(), &exporter, &server, &signature)?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```

#![warn(missing_docs)]

mod encoding;
mod kdf;

pub mod fingerprint;
pub mod handshake;
pub mod join;
pub mod key_link;
pub mod keys;
pub mod level;
pub mod passkey;
pub mod phrase;
pub mod trust;
pub mod wordlist;

pub use encoding::LengthError;
pub use fingerprint::{Fingerprint, FingerprintError, SixWords};
pub use join::{Host, JoinCode, JoinCodeError, JoinLink, LinkError};
pub use key_link::{LinkKeyError, LinkStatement};
pub use keys::{InvalidSignature, KeyError, PublicKey, Signature, SigningKey};
pub use level::{
    LevelSearch, LevelTooLow, identity_level, search_identity_level, verify_identity_level,
};
pub use phrase::{
    ImportCandidate, ImportReading, PhraseError, PhraseV1, PhraseV2, PhraseVersion, RecoveryPhrase,
    import_phrase,
};
pub use trust::{PinCheck, RotationError, RotationStatement, check_pin};
/// The wrapper around secret values this crate hands out: it wipes its
/// content when dropped.
pub use zeroize::Zeroizing;

/// Fills a buffer from the operating system's random-number generator.
///
/// # Panics
///
/// If the operating system cannot provide random bytes. Every caller makes
/// a secret from the result, and there is no sound way to continue without
/// it (`docs/rust-rewrite/04-connection-security.md`, finding F21).
#[cfg(feature = "os-rng")]
pub(crate) fn fill_random(buffer: &mut [u8]) {
    getrandom::fill(buffer).expect("the operating system's random-number generator failed");
}
