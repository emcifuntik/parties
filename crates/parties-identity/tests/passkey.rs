//! The identity key kept in a passkey: from the PRF output to the key.

mod common;

use common::{array, key, list, public_key, text, to_hex, vectors};
use parties_identity::passkey::{self, PASSKEY_INFO, PRF_OUTPUT_LEN, PasskeyMismatch};
use parties_identity::phrase::RECOVERY_ENTROPY_LEN;
use parties_identity::{Fingerprint, PhraseV2};

#[test]
fn label_and_lengths_are_pinned() {
    assert_eq!(PASSKEY_INFO, b"parties/passkey/identity/v1");
    assert_eq!(PRF_OUTPUT_LEN, 32);
    assert_eq!(RECOVERY_ENTROPY_LEN, 16);
}

/// `HKDF-SHA256(prf_output, info = "parties/passkey/identity/v1")` cut to 16
/// bytes, then the version 2 derivation: computed by
/// `tests/vectors/generate.py`.
#[test]
fn passkey_derivation_matches_the_vectors() {
    let file = vectors("identity.json");
    let cases = list(&file, "passkey");
    assert!(cases.len() >= 3);
    for case in cases {
        let prf_output: [u8; 32] = array(case, "prf_output");

        let phrase = passkey::recovery_phrase(&prf_output);
        assert_eq!(to_hex(phrase.entropy()), text(case, "entropy"));
        assert_eq!(phrase.word_count(), 12);
        assert_eq!(phrase.expose().as_str(), text(case, "phrase"));

        let (unlocked_phrase, signing_key) = passkey::unlock(&prf_output, None).expect("no record");
        assert_eq!(unlocked_phrase.entropy(), phrase.entropy());
        assert_eq!(signing_key.public_key(), public_key(case, "public_key"));
        assert_eq!(
            signing_key.fingerprint().to_string(),
            text(case, "fingerprint")
        );
    }
}

/// "The identity still has a recovery phrase": the words shown for a
/// passkey-born identity restore the same key without the passkey.
#[test]
fn the_phrase_of_a_passkey_identity_restores_the_same_key() {
    let prf_output = [0x42; 32];
    let (phrase, signing_key) = passkey::unlock(&prf_output, None).expect("no record");
    let written_down = phrase.expose();

    let restored = PhraseV2::parse(&written_down).expect("valid phrase");
    assert_eq!(
        restored.signing_key().public_key(),
        signing_key.public_key()
    );
}

#[test]
fn same_passkey_same_identity_other_passkey_other_identity() {
    let (_, first) = passkey::unlock(&[1; 32], None).expect("no record");
    let (_, again) = passkey::unlock(&[1; 32], None).expect("no record");
    let (_, other) = passkey::unlock(&[2; 32], None).expect("no record");
    assert_eq!(first.public_key(), again.public_key());
    assert_ne!(first.public_key(), other.public_key());

    // One changed bit of the PRF output is another identity.
    let mut flipped = [1u8; 32];
    flipped[31] ^= 0x80;
    let (_, flipped) = passkey::unlock(&flipped, None).expect("no record");
    assert_ne!(first.public_key(), flipped.public_key());
}

/// Section 7.3: the client refuses a derived key that is not the one on
/// record, instead of silently becoming a different identity.
#[test]
fn unlock_checks_the_expected_fingerprint() {
    let prf_output = [0x42; 32];
    let (_, signing_key) = passkey::unlock(&prf_output, None).expect("no record");
    let expected = signing_key.fingerprint();

    let (_, again) = passkey::unlock(&prf_output, Some(&expected)).expect("the recorded identity");
    assert_eq!(again.public_key(), signing_key.public_key());

    // The provider returned something else (a defect, another passkey).
    let error = passkey::unlock(&[0x43; 32], Some(&expected))
        .err()
        .expect("another identity");
    assert_eq!(error.expected, expected);
    assert_ne!(error.found, expected);
    let (_, actual) = passkey::unlock(&[0x43; 32], None).expect("no record");
    assert_eq!(error.found, actual.fingerprint());

    // The record names some unrelated identity.
    let unrelated: Fingerprint = key(9).fingerprint();
    assert_eq!(
        passkey::unlock(&prf_output, Some(&unrelated)).err(),
        Some(PasskeyMismatch {
            expected: unrelated,
            found: expected
        })
    );
}

#[test]
fn mismatch_message_names_fingerprints_only() {
    let expected = key(9).fingerprint();
    let error = passkey::unlock(&[0x42; 32], Some(&expected))
        .err()
        .expect("another identity");
    let message = error.to_string();
    assert!(message.contains(&expected.to_string()));
    assert!(message.contains(&error.found.to_string()));
}
