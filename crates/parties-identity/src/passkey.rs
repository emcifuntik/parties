//! The identity key kept in a passkey
//! (`docs/rust-rewrite/13-profiles-access-and-hardware-keys.md` section 7.2).
//!
//! A passkey cannot hold an arbitrary key, but through the PRF extension of
//! WebAuthn it returns 32 secret bytes that depend only on the passkey and
//! on an input the application chooses. Those bytes become the recovery
//! entropy of an ordinary version 2 identity:
//!
//! ```text
//! entropy  = HKDF-SHA256(salt = none, ikm = prf_output,
//!                        info = "parties/passkey/identity/v1", 16 bytes)
//! identity = the version 2 derivation over the phrase of that entropy
//! ```
//!
//! So the same passkey gives the same phrase, the same key and the same
//! fingerprint on every device, the phrase can still be written down, and a
//! server sees nothing special.
//!
//! This module starts at the PRF output. Creating the passkey, asking it for
//! an assertion and choosing the PRF input are the platform's part and are
//! not in this crate.

use crate::fingerprint::Fingerprint;
use crate::kdf::hkdf_sha256;
use crate::keys::SigningKey;
use crate::phrase::{PhraseV2, RECOVERY_ENTROPY_LEN};

/// HKDF `info` of the passkey derivation.
pub const PASSKEY_INFO: &[u8] = b"parties/passkey/identity/v1";

/// Length of the PRF output a passkey returns, in bytes.
pub const PRF_OUTPUT_LEN: usize = 32;

/// The passkey gave a different identity than the one on record.
///
/// The client keeps the expected fingerprint next to its passkey record and
/// refuses the derived key instead of silently becoming someone else
/// (section 7.3 of the document).
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
#[error("the passkey gives identity {found}, not the expected {expected}")]
pub struct PasskeyMismatch {
    /// The fingerprint stored with the passkey record.
    pub expected: Fingerprint,
    /// The fingerprint the PRF output leads to.
    pub found: Fingerprint,
}

/// The recovery phrase of the identity a passkey holds.
///
/// `prf_output` is what the passkey returned for the fixed PRF input. It is
/// a secret; the caller wipes its copy.
///
/// ```
/// use parties_identity::passkey;
///
/// let phrase = passkey::recovery_phrase(&[0x42; 32]);
/// assert_eq!(phrase.word_count(), 12);
/// ```
#[must_use]
pub fn recovery_phrase(prf_output: &[u8; PRF_OUTPUT_LEN]) -> PhraseV2 {
    let entropy = hkdf_sha256::<RECOVERY_ENTROPY_LEN>(prf_output, PASSKEY_INFO);
    PhraseV2::from_recovery_entropy(&entropy)
}

/// Derives the identity a passkey holds: its recovery phrase and its key.
///
/// With `expected`, the fingerprint stored when the passkey was created,
/// the result is refused unless it is that identity. Pass `None` only when
/// the passkey is new or is being restored on a device with no record.
///
/// # Errors
///
/// [`PasskeyMismatch`] if the derived fingerprint is not the expected one.
pub fn unlock(
    prf_output: &[u8; PRF_OUTPUT_LEN],
    expected: Option<&Fingerprint>,
) -> Result<(PhraseV2, SigningKey), PasskeyMismatch> {
    let phrase = recovery_phrase(prf_output);
    let signing_key = phrase.signing_key();
    let found = signing_key.fingerprint();
    match expected {
        Some(expected) if *expected != found => Err(PasskeyMismatch {
            expected: *expected,
            found,
        }),
        _ => Ok((phrase, signing_key)),
    }
}
