//! Server trust: the pin comparison and rotation statements.

mod common;

use common::{
    bytes, flip_bit, key, member, public_key, signature, signing_key, text, to_hex, vectors,
};
use parties_identity::trust::{
    MAX_ROTATION_CHAIN, MAX_ROTATION_STATEMENTS, ROTATION_LABEL, ROTATION_MESSAGE_LEN,
    ROTATION_STATEMENT_LEN, rotation_message,
};
use parties_identity::{
    InvalidSignature, KeyError, LengthError, PinCheck, PublicKey, RotationError, RotationStatement,
    Signature, check_pin,
};

fn public(byte: u8) -> PublicKey {
    key(byte).public_key()
}

/// The statement by which the key of seed `old` vouches for that of `new`.
fn rotation(old: u8, new: u8) -> RotationStatement {
    RotationStatement::sign(&key(old), &public(new)).expect("two different keys")
}

// ---------------------------------------------------------------------------
// The pin comparison
// ---------------------------------------------------------------------------

#[test]
fn pinned_key_matches_itself() {
    assert_eq!(
        check_pin(Some(&public(1)), &public(1), &[]),
        PinCheck::Match
    );
    // Statements do not matter when the key is the pinned one.
    assert_eq!(
        check_pin(Some(&public(1)), &public(1), &[rotation(1, 2)]),
        PinCheck::Match
    );
}

#[test]
fn no_pin_means_ask_the_user() {
    assert_eq!(check_pin(None, &public(1), &[]), PinCheck::Unknown);
    // A rotation statement cannot stand in for a pin that does not exist.
    assert_eq!(
        check_pin(None, &public(2), &[rotation(1, 2)]),
        PinCheck::Unknown
    );
}

/// Finding F10: a changed key is a hard failure, not a question.
#[test]
fn another_key_is_a_mismatch() {
    assert_eq!(
        check_pin(Some(&public(1)), &public(2), &[]),
        PinCheck::Mismatch
    );
    // Keys that differ in a single bit of their seed, and every seed tried.
    for seed in 2..=40 {
        assert_eq!(
            check_pin(Some(&public(1)), &public(seed), &[]),
            PinCheck::Mismatch
        );
    }
}

#[test]
fn rotation_from_the_pin_is_followed() {
    assert_eq!(
        check_pin(Some(&public(1)), &public(2), &[rotation(1, 2)]),
        PinCheck::Rotated
    );
}

#[test]
fn rotation_chains_are_followed_in_any_order() {
    let chain = [rotation(1, 2), rotation(2, 3), rotation(3, 4)];
    assert_eq!(
        check_pin(Some(&public(1)), &public(4), &chain),
        PinCheck::Rotated
    );
    // A client that pinned a later key uses the tail of the history.
    assert_eq!(
        check_pin(Some(&public(2)), &public(4), &chain),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(3)), &public(4), &chain),
        PinCheck::Rotated
    );
    // An intermediate key is vouched for as well.
    assert_eq!(
        check_pin(Some(&public(1)), &public(3), &chain),
        PinCheck::Rotated
    );

    let reversed = [rotation(3, 4), rotation(2, 3), rotation(1, 2)];
    assert_eq!(
        check_pin(Some(&public(1)), &public(4), &reversed),
        PinCheck::Rotated
    );
    let shuffled = [rotation(2, 3), rotation(1, 2), rotation(3, 4)];
    assert_eq!(
        check_pin(Some(&public(1)), &public(4), &shuffled),
        PinCheck::Rotated
    );
}

#[test]
fn rotation_never_runs_backwards() {
    let chain = [rotation(1, 2), rotation(2, 3)];
    // The new key does not vouch for the old one.
    assert_eq!(
        check_pin(Some(&public(3)), &public(1), &chain),
        PinCheck::Mismatch
    );
    assert_eq!(
        check_pin(Some(&public(2)), &public(1), &chain),
        PinCheck::Mismatch
    );
}

#[test]
fn rotation_that_does_not_start_at_the_pin_is_a_mismatch() {
    // Signed by a key the client never trusted.
    assert_eq!(
        check_pin(Some(&public(1)), &public(3), &[rotation(2, 3)]),
        PinCheck::Mismatch
    );
    // A chain with a gap.
    assert_eq!(
        check_pin(
            Some(&public(1)),
            &public(4),
            &[rotation(1, 2), rotation(3, 4)]
        ),
        PinCheck::Mismatch
    );
    // A chain that leads elsewhere.
    assert_eq!(
        check_pin(
            Some(&public(1)),
            &public(9),
            &[rotation(1, 2), rotation(2, 3)]
        ),
        PinCheck::Mismatch
    );
    // An attacker's own statement about its own keys.
    assert_eq!(
        check_pin(
            Some(&public(1)),
            &public(8),
            &[rotation(7, 8), rotation(8, 7)]
        ),
        PinCheck::Mismatch
    );
}

#[test]
fn unrelated_and_branching_statements_are_tolerated() {
    let statements = [
        rotation(7, 8),
        rotation(1, 2),
        rotation(1, 5),
        rotation(2, 3),
    ];
    assert_eq!(
        check_pin(Some(&public(1)), &public(3), &statements),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(5), &statements),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(8), &statements),
        PinCheck::Mismatch
    );
    // A loop in the statements ends.
    let cycle = [rotation(1, 2), rotation(2, 1), rotation(2, 3)];
    assert_eq!(
        check_pin(Some(&public(1)), &public(3), &cycle),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(4), &cycle),
        PinCheck::Mismatch
    );
}

#[test]
fn chains_longer_than_the_limit_are_not_followed() {
    assert_eq!(MAX_ROTATION_CHAIN, 16);
    // 1 -> 2 -> ... -> 18: seventeen statements in a row.
    let chain: Vec<RotationStatement> = (1..=17).map(|seed| rotation(seed, seed + 1)).collect();
    assert_eq!(chain.len(), MAX_ROTATION_CHAIN + 1);
    assert_eq!(
        check_pin(Some(&public(1)), &public(18), &chain),
        PinCheck::Mismatch
    );
    // Sixteen in a row are followed, from either end of the history and in
    // the worst order.
    assert_eq!(
        check_pin(Some(&public(1)), &public(17), &chain),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(2)), &public(18), &chain),
        PinCheck::Rotated
    );
    let mut reversed = chain.clone();
    reversed.reverse();
    assert_eq!(
        check_pin(Some(&public(1)), &public(17), &reversed),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(18), &reversed),
        PinCheck::Mismatch
    );
    // The pinned key itself still matches, whatever comes with it.
    assert_eq!(
        check_pin(Some(&public(1)), &public(1), &chain),
        PinCheck::Match
    );
}

/// A long history must not lock out a client that needs one step of it.
#[test]
fn a_short_chain_is_found_among_many_statements() {
    // Forty statements of other servers, then the one that matters.
    let mut statements: Vec<RotationStatement> =
        (100..140).map(|seed| rotation(seed, seed + 1)).collect();
    statements.push(rotation(1, 2));
    assert!(statements.len() > MAX_ROTATION_STATEMENTS);
    assert_eq!(
        check_pin(Some(&public(1)), &public(2), &statements),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(3), &statements),
        PinCheck::Mismatch
    );
    // A server's own long history: the client that pinned a recent key
    // follows the tail.
    let history: Vec<RotationStatement> = (1..=40).map(|seed| rotation(seed, seed + 1)).collect();
    assert_eq!(
        check_pin(Some(&public(30)), &public(41), &history),
        PinCheck::Rotated
    );
    assert_eq!(
        check_pin(Some(&public(1)), &public(41), &history),
        PinCheck::Mismatch
    );
}

// ---------------------------------------------------------------------------
// Rotation statements
// ---------------------------------------------------------------------------

#[test]
fn rotation_statement_matches_the_vector() {
    assert_eq!(ROTATION_LABEL, b"parties-server-rotation-v1");
    assert_eq!(ROTATION_MESSAGE_LEN, 26 + 1 + 32 + 32);
    assert_eq!(ROTATION_STATEMENT_LEN, 128);

    let file = vectors("statements.json");
    let case = member(&file, "rotation");
    let old_key = signing_key(case, "old_seed");
    let old_public = public_key(case, "old_key");
    let new_public = public_key(case, "new_key");
    assert_eq!(old_key.public_key(), old_public);

    assert_eq!(
        to_hex(&rotation_message(&old_public, &new_public)),
        text(case, "message")
    );
    let statement = RotationStatement::sign(&old_key, &new_public).expect("two different keys");
    assert_eq!(statement.signature(), &signature(case, "signature"));
    assert_eq!(statement.old_key(), &old_public);
    assert_eq!(statement.new_key(), &new_public);
    assert_eq!(to_hex(&statement.to_bytes()), text(case, "statement"));

    let read = RotationStatement::from_bytes(&bytes(case, "statement")).expect("valid statement");
    assert_eq!(read, statement);
    let from_parts =
        RotationStatement::from_parts(old_public, new_public, signature(case, "signature"));
    assert_eq!(from_parts, Ok(statement));
}

#[test]
fn rotation_message_is_label_zero_byte_and_both_keys() {
    let message = rotation_message(&public(1), &public(2));
    assert_eq!(&message[..26], b"parties-server-rotation-v1");
    assert_eq!(message[26], 0);
    assert_eq!(&message[27..59], public(1).as_bytes());
    assert_eq!(&message[59..], public(2).as_bytes());
}

#[test]
fn rotation_to_the_same_key_is_refused() {
    assert_eq!(
        RotationStatement::sign(&key(1), &public(1)),
        Err(RotationError::SameKey)
    );
    let signature = key(1).sign(&rotation_message(&public(1), &public(1)));
    assert_eq!(
        RotationStatement::from_parts(public(1), public(1), signature),
        Err(RotationError::SameKey)
    );
}

#[test]
fn rotation_with_a_wrong_signature_is_refused() {
    let good = rotation(1, 2);
    let invalid = Err(RotationError::Signature(InvalidSignature));

    // Signed by the new key instead of the old one.
    let by_new_key = key(2).sign(&rotation_message(&public(1), &public(2)));
    assert_eq!(
        RotationStatement::from_parts(public(1), public(2), by_new_key),
        invalid
    );
    // Signed by a third key.
    let by_stranger = key(3).sign(&rotation_message(&public(1), &public(2)));
    assert_eq!(
        RotationStatement::from_parts(public(1), public(2), by_stranger),
        invalid
    );
    // The right signature, attached to another new key or to swapped keys.
    assert_eq!(
        RotationStatement::from_parts(public(1), public(3), *good.signature()),
        invalid
    );
    assert_eq!(
        RotationStatement::from_parts(public(2), public(1), *good.signature()),
        invalid
    );
    // A signature over the two keys without the label.
    let mut unlabelled = Vec::new();
    unlabelled.extend_from_slice(public(1).as_bytes());
    unlabelled.extend_from_slice(public(2).as_bytes());
    assert_eq!(
        RotationStatement::from_parts(public(1), public(2), key(1).sign(&unlabelled)),
        invalid
    );
    assert_eq!(
        RotationStatement::from_parts(public(1), public(2), Signature::from_bytes([0; 64])),
        invalid
    );
}

#[test]
fn encoded_rotation_refuses_truncation_and_any_altered_bit() {
    let good = rotation(1, 2).to_bytes();
    assert_eq!(
        RotationStatement::from_bytes(&good).map(|read| read.to_bytes()),
        Ok(good)
    );

    for length in [0, 1, 64, 127, 129, 256] {
        let mut bytes = good.to_vec();
        bytes.resize(length, 0);
        assert_eq!(
            RotationStatement::from_bytes(&bytes),
            Err(RotationError::Length(LengthError {
                expected: 128,
                found: length
            })),
            "{length} bytes"
        );
    }

    for bit in 0..(128 * 8) {
        let altered = flip_bit(&good, bit);
        let result = RotationStatement::from_bytes(&altered);
        assert!(
            matches!(
                result,
                Err(RotationError::Signature(_) | RotationError::Key(_))
            ),
            "bit {bit}: {result:?}"
        );
    }
}

#[test]
fn encoded_rotation_refuses_keys_that_are_no_keys() {
    let good = rotation(1, 2).to_bytes();

    // The identity point as the old key: anyone can "sign" for it.
    let mut weak_old = good;
    weak_old[..32].copy_from_slice(&{
        let mut identity = [0u8; 32];
        identity[0] = 1;
        identity
    });
    assert_eq!(
        RotationStatement::from_bytes(&weak_old),
        Err(RotationError::Key(KeyError::WeakKey))
    );

    let mut invalid_new = good;
    invalid_new[32..64].copy_from_slice(&{
        let mut not_a_point = [0u8; 32];
        not_a_point[0] = 2;
        not_a_point
    });
    assert_eq!(
        RotationStatement::from_bytes(&invalid_new),
        Err(RotationError::Key(KeyError::InvalidPoint))
    );
}

// ---------------------------------------------------------------------------
// Lists of rotation statements
// ---------------------------------------------------------------------------

#[test]
fn rotation_lists_round_trip() {
    assert_eq!(MAX_ROTATION_STATEMENTS, 32);
    assert_eq!(RotationStatement::list_from_bytes(&[]), Ok(Vec::new()));
    assert_eq!(RotationStatement::list_to_bytes(&[]), Vec::<u8>::new());

    let statements = [rotation(1, 2), rotation(2, 3), rotation(7, 8)];
    let encoded = RotationStatement::list_to_bytes(&statements);
    assert_eq!(encoded.len(), 3 * 128);
    assert_eq!(&encoded[..128], &statements[0].to_bytes());
    assert_eq!(&encoded[256..], &statements[2].to_bytes());
    assert_eq!(
        RotationStatement::list_from_bytes(&encoded),
        Ok(statements.to_vec())
    );

    // The longest list that is accepted.
    let longest: Vec<RotationStatement> = (1..=32).map(|seed| rotation(seed, seed + 1)).collect();
    let encoded = RotationStatement::list_to_bytes(&longest);
    assert_eq!(RotationStatement::list_from_bytes(&encoded), Ok(longest));
}

#[test]
fn rotation_lists_of_a_wrong_length_are_refused() {
    let encoded = RotationStatement::list_to_bytes(&[rotation(1, 2), rotation(2, 3)]);
    for length in [1, 127, 129, 255, 257] {
        let mut bytes = encoded.clone();
        bytes.resize(length, 0);
        assert_eq!(
            RotationStatement::list_from_bytes(&bytes),
            Err(RotationError::ListLength { found: length }),
            "{length} bytes"
        );
    }
}

/// The count is refused before anything is verified: the list below is 33
/// statements of zero bytes, none of which would verify.
#[test]
fn rotation_lists_that_are_too_long_are_refused_unverified() {
    let garbage = vec![0u8; 33 * 128];
    assert_eq!(
        RotationStatement::list_from_bytes(&garbage),
        Err(RotationError::TooMany { found: 33 })
    );
    let huge = vec![0u8; 10_000 * 128];
    assert_eq!(
        RotationStatement::list_from_bytes(&huge),
        Err(RotationError::TooMany { found: 10_000 })
    );
    // Within the limit, the same garbage is refused for what it is.
    let within = vec![0u8; 32 * 128];
    assert_eq!(
        RotationStatement::list_from_bytes(&within),
        Err(RotationError::Key(KeyError::WeakKey))
    );
}

#[test]
fn one_bad_statement_spoils_the_list() {
    let mut encoded = RotationStatement::list_to_bytes(&[rotation(1, 2), rotation(2, 3)]);
    // Alter the signature of the second statement.
    let last = encoded.len() - 1;
    encoded[last] ^= 1;
    assert_eq!(
        RotationStatement::list_from_bytes(&encoded),
        Err(RotationError::Signature(InvalidSignature))
    );
}
