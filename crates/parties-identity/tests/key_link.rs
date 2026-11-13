//! Linking a second key to an account: the statement, the new key's proof
//! and the optional approval by the existing key.

mod common;

use common::{
    exporter, flip_bit, key, member, public_key, signature, signing_key, text, to_hex, vectors,
};
use parties_identity::handshake::{Exporter, sign_server_proof, sign_user_auth};
use parties_identity::key_link::{LINK_KEY_LABEL, LINK_KEY_MESSAGE_LEN};
use parties_identity::{InvalidSignature, LinkKeyError, LinkStatement, PublicKey, Signature};

const SERVER: u8 = 1;
const ACCOUNT: u8 = 2;
const NEW: u8 = 3;

fn public(byte: u8) -> PublicKey {
    key(byte).public_key()
}

fn exporter_of(byte: u8) -> Exporter {
    Exporter::from_bytes([byte; 32])
}

fn statement(exporter: u8, server: u8, account: u8, new: u8) -> LinkStatement {
    LinkStatement::new(
        &exporter_of(exporter),
        &public(server),
        &public(account),
        &public(new),
    )
    .expect("two different keys")
}

fn base() -> LinkStatement {
    statement(0xA0, SERVER, ACCOUNT, NEW)
}

const INVALID: Result<(), LinkKeyError> = Err(LinkKeyError::Signature(InvalidSignature));

#[test]
fn statement_matches_the_vector() {
    assert_eq!(LINK_KEY_LABEL, b"parties-link-key-v1");
    assert_eq!(LINK_KEY_MESSAGE_LEN, 19 + 1 + 32 + 32 + 32 + 32);

    let file = vectors("statements.json");
    let case = member(&file, "key_link");
    let account_key = signing_key(case, "account_seed");
    let new_key = signing_key(case, "new_seed");
    assert_eq!(account_key.public_key(), public_key(case, "account_key"));
    assert_eq!(new_key.public_key(), public_key(case, "new_key"));

    let statement = LinkStatement::new(
        &exporter(case, "exporter"),
        &public_key(case, "server_identity"),
        &account_key.public_key(),
        &new_key.public_key(),
    )
    .expect("two different keys");
    assert_eq!(to_hex(&statement.message()), text(case, "message"));
    assert_eq!(statement.account_key(), &account_key.public_key());
    assert_eq!(statement.new_key(), &new_key.public_key());

    let proof = statement.sign_with_new_key(&new_key).expect("the new key");
    assert_eq!(proof, signature(case, "proof"));
    assert_eq!(statement.verify_new_key(&proof), Ok(()));

    let approval = statement
        .sign_with_account_key(&account_key)
        .expect("the account key");
    assert_eq!(approval, signature(case, "approval"));
    assert_eq!(statement.verify_account_key(&approval), Ok(()));
}

#[test]
fn message_is_label_zero_byte_and_fields() {
    let message = base().message();
    assert_eq!(&message[..19], b"parties-link-key-v1");
    assert_eq!(message[19], 0);
    assert_eq!(&message[20..52], exporter_of(0xA0).as_bytes());
    assert_eq!(&message[52..84], public(SERVER).as_bytes());
    assert_eq!(&message[84..116], public(ACCOUNT).as_bytes());
    assert_eq!(&message[116..], public(NEW).as_bytes());
}

#[test]
fn a_key_cannot_be_linked_to_itself() {
    assert_eq!(
        LinkStatement::new(
            &exporter_of(0xA0),
            &public(SERVER),
            &public(ACCOUNT),
            &public(ACCOUNT)
        ),
        Err(LinkKeyError::SameKey)
    );
}

#[test]
fn each_role_signs_with_its_own_key_only() {
    let statement = base();
    assert_eq!(
        statement.sign_with_new_key(&key(ACCOUNT)),
        Err(LinkKeyError::WrongKey)
    );
    assert_eq!(
        statement.sign_with_new_key(&key(9)),
        Err(LinkKeyError::WrongKey)
    );
    assert_eq!(
        statement.sign_with_account_key(&key(NEW)),
        Err(LinkKeyError::WrongKey)
    );
    assert_eq!(
        statement.sign_with_account_key(&key(9)),
        Err(LinkKeyError::WrongKey)
    );
}

/// The proof must come from the new key: an account cannot attach a key it
/// does not hold by signing with the key it has.
#[test]
fn proof_by_another_key_is_refused() {
    let statement = base();
    let message = statement.message();
    assert_eq!(
        statement.verify_new_key(&key(ACCOUNT).sign(&message)),
        INVALID
    );
    assert_eq!(
        statement.verify_new_key(&key(SERVER).sign(&message)),
        INVALID
    );
    assert_eq!(statement.verify_new_key(&key(9).sign(&message)), INVALID);
    // And the approval must come from the account key.
    assert_eq!(
        statement.verify_account_key(&key(NEW).sign(&message)),
        INVALID
    );
}

/// A proof and an approval are signatures over the same bytes by different
/// keys; neither passes for the other.
#[test]
fn proof_and_approval_are_not_interchangeable() {
    let statement = base();
    let proof = statement.sign_with_new_key(&key(NEW)).expect("new key");
    let approval = statement
        .sign_with_account_key(&key(ACCOUNT))
        .expect("account key");
    assert_ne!(proof, approval);
    assert_eq!(statement.verify_new_key(&approval), INVALID);
    assert_eq!(statement.verify_account_key(&proof), INVALID);
}

#[test]
fn proof_fails_on_another_connection() {
    let proof = base().sign_with_new_key(&key(NEW)).expect("new key");
    let replayed = statement(0xA1, SERVER, ACCOUNT, NEW);
    assert_eq!(replayed.verify_new_key(&proof), INVALID);
}

#[test]
fn proof_fails_on_another_server() {
    let proof = base().sign_with_new_key(&key(NEW)).expect("new key");
    let relayed = statement(0xA0, 9, ACCOUNT, NEW);
    assert_eq!(relayed.verify_new_key(&proof), INVALID);
}

/// The proof names the account. A server operator cannot attach a member's
/// new key to another account with it.
#[test]
fn proof_fails_for_another_account() {
    let proof = base().sign_with_new_key(&key(NEW)).expect("new key");
    let other_account = statement(0xA0, SERVER, 9, NEW);
    assert_eq!(other_account.verify_new_key(&proof), INVALID);
}

#[test]
fn proof_fails_for_another_new_key() {
    let proof = base().sign_with_new_key(&key(NEW)).expect("new key");
    let other_key = statement(0xA0, SERVER, ACCOUNT, 9);
    assert_eq!(other_key.verify_new_key(&proof), INVALID);
    // Swapping the two roles is another statement as well.
    let swapped = statement(0xA0, SERVER, NEW, ACCOUNT);
    assert_eq!(swapped.verify_account_key(&proof), INVALID);
}

#[test]
fn proof_fails_when_any_bit_is_altered() {
    let statement = base();
    let proof = statement.sign_with_new_key(&key(NEW)).expect("new key");
    for bit in 0..512 {
        let altered = Signature::from_bytes(flip_bit(proof.as_bytes(), bit));
        assert_eq!(statement.verify_new_key(&altered), INVALID, "bit {bit}");
    }
    assert_eq!(statement.verify_new_key(&proof), Ok(()));
}

/// The handshake signatures have their own labels, so a login or a server
/// proof made on the same connection is not a link proof.
#[test]
fn handshake_signatures_are_not_link_proofs() {
    let statement = base();
    let exporter = exporter_of(0xA0);
    let login = sign_user_auth(&key(NEW), &exporter, &public(SERVER));
    assert_eq!(statement.verify_new_key(&login), INVALID);
    let server_proof = sign_server_proof(&key(NEW), &exporter);
    assert_eq!(statement.verify_new_key(&server_proof), INVALID);
}
