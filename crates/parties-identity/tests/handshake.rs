//! The proofs of the handshake: golden vectors, and every way a proof must
//! fail (wrong exporter, wrong server key, wrong user key, truncated input,
//! altered signature).

mod common;

use common::{
    array, exporter, flip_bit, key, list, member, number32, public_key, signature, signing_key,
    text, to_hex, vectors,
};
use parties_identity::handshake::{
    EXPORTER_CONTEXT, EXPORTER_LABEL, EXPORTER_LEN, Exporter, InvalidPasswordProof,
    JOIN_PASSWORD_LABEL, KdfParamsError, PASSWORD_KEY_LEN, PASSWORD_PROOF_LEN, PasswordKdfParams,
    PasswordKey, SERVER_PROOF_LABEL, SERVER_PROOF_MESSAGE_LEN, USER_AUTH_LABEL,
    USER_AUTH_MESSAGE_LEN, server_proof_message, sign_server_proof, sign_user_auth,
    user_auth_message, verify_server_proof, verify_user_auth,
};
use parties_identity::{InvalidSignature, LengthError, Signature};

fn exporter_of(byte: u8) -> Exporter {
    Exporter::from_bytes([byte; 32])
}

// ---------------------------------------------------------------------------
// Labels and layouts: these bytes are the protocol
// ---------------------------------------------------------------------------

#[test]
fn labels_are_the_documented_strings() {
    assert_eq!(EXPORTER_LABEL, b"EXPORTER-parties-auth-v2");
    assert_eq!(EXPORTER_CONTEXT, b"");
    assert_eq!(EXPORTER_LEN, 32);
    assert_eq!(SERVER_PROOF_LABEL, b"parties-server-proof-v2");
    assert_eq!(USER_AUTH_LABEL, b"parties-auth-v2");
    assert_eq!(JOIN_PASSWORD_LABEL, b"parties-join-pw-v2");
    assert_eq!(PASSWORD_KEY_LEN, 32);
    assert_eq!(PASSWORD_PROOF_LEN, 32);
}

#[test]
fn messages_are_label_zero_byte_and_fields() {
    let exporter = exporter_of(0xE1);
    let server = key(1).public_key();
    let user = key(2).public_key();

    let message = server_proof_message(&exporter);
    assert_eq!(message.len(), SERVER_PROOF_MESSAGE_LEN);
    assert_eq!(SERVER_PROOF_MESSAGE_LEN, 23 + 1 + 32);
    assert_eq!(&message[..23], b"parties-server-proof-v2");
    assert_eq!(message[23], 0);
    assert_eq!(&message[24..], exporter.as_bytes());

    let message = user_auth_message(&exporter, &server, &user);
    assert_eq!(message.len(), USER_AUTH_MESSAGE_LEN);
    assert_eq!(USER_AUTH_MESSAGE_LEN, 15 + 1 + 32 + 32 + 32);
    assert_eq!(&message[..15], b"parties-auth-v2");
    assert_eq!(message[15], 0);
    assert_eq!(&message[16..48], exporter.as_bytes());
    assert_eq!(&message[48..80], server.as_bytes());
    assert_eq!(&message[80..], user.as_bytes());
}

// ---------------------------------------------------------------------------
// Golden vectors
// ---------------------------------------------------------------------------

#[test]
fn handshake_matches_the_vectors() {
    let file = vectors("statements.json");
    let case = member(&file, "handshake");
    let exporter = exporter(case, "exporter");
    let server_key = signing_key(case, "server_seed");
    let user_key = signing_key(case, "user_seed");
    let server = public_key(case, "server_identity");
    let user = public_key(case, "user_key");
    assert_eq!(server_key.public_key(), server);
    assert_eq!(user_key.public_key(), user);

    assert_eq!(
        to_hex(&server_proof_message(&exporter)),
        text(case, "server_proof_message")
    );
    let proof = sign_server_proof(&server_key, &exporter);
    assert_eq!(proof, signature(case, "server_proof"));
    assert_eq!(verify_server_proof(&server, &exporter, &proof), Ok(()));

    assert_eq!(
        to_hex(&user_auth_message(&exporter, &server, &user)),
        text(case, "user_auth_message")
    );
    let login = sign_user_auth(&user_key, &exporter, &server);
    assert_eq!(login, signature(case, "signature"));
    assert_eq!(verify_user_auth(&user, &exporter, &server, &login), Ok(()));
}

// ---------------------------------------------------------------------------
// server_proof: what must fail
// ---------------------------------------------------------------------------

#[test]
fn server_proof_fails_on_another_connection() {
    let server_key = key(1);
    let proof = sign_server_proof(&server_key, &exporter_of(0xA0));
    assert_eq!(
        verify_server_proof(&server_key.public_key(), &exporter_of(0xA1), &proof),
        Err(InvalidSignature)
    );
}

#[test]
fn server_proof_fails_for_another_server_key() {
    let exporter = exporter_of(0xA0);
    let proof = sign_server_proof(&key(1), &exporter);
    // An impostor cannot reuse the real server's proof under its own key,
    // and its own proof does not verify under the pinned key.
    assert_eq!(
        verify_server_proof(&key(9).public_key(), &exporter, &proof),
        Err(InvalidSignature)
    );
    let impostor_proof = sign_server_proof(&key(9), &exporter);
    assert_eq!(
        verify_server_proof(&key(1).public_key(), &exporter, &impostor_proof),
        Err(InvalidSignature)
    );
}

#[test]
fn server_proof_fails_when_any_bit_is_altered() {
    let exporter = exporter_of(0xA0);
    let server_key = key(1);
    let proof = sign_server_proof(&server_key, &exporter);
    for bit in 0..512 {
        let altered = Signature::from_bytes(flip_bit(proof.as_bytes(), bit));
        assert_eq!(
            verify_server_proof(&server_key.public_key(), &exporter, &altered),
            Err(InvalidSignature),
            "signature bit {bit}"
        );
    }
    for bit in 0..256 {
        let altered = Exporter::from_bytes(flip_bit(exporter.as_bytes(), bit));
        assert_eq!(
            verify_server_proof(&server_key.public_key(), &altered, &proof),
            Err(InvalidSignature),
            "exporter bit {bit}"
        );
    }
}

// ---------------------------------------------------------------------------
// The user's signature: what must fail
// ---------------------------------------------------------------------------

/// Finding F1: a login made on one connection is worthless on another, even
/// to the same server.
#[test]
fn user_signature_fails_on_another_connection() {
    let (user_key, server) = (key(2), key(1).public_key());
    let login = sign_user_auth(&user_key, &exporter_of(0xA0), &server);
    assert_eq!(
        verify_user_auth(&user_key.public_key(), &exporter_of(0xA1), &server, &login),
        Err(InvalidSignature)
    );
}

/// Finding F1, the relay: a server the user logs in to cannot use that
/// login on a second server, even if it could force the same exporter.
#[test]
fn user_signature_fails_for_another_server_key() {
    let exporter = exporter_of(0xA0);
    let user_key = key(2);
    let login = sign_user_auth(&user_key, &exporter, &key(1).public_key());
    assert_eq!(
        verify_user_auth(
            &user_key.public_key(),
            &exporter,
            &key(9).public_key(),
            &login
        ),
        Err(InvalidSignature)
    );
}

#[test]
fn user_signature_fails_for_another_user_key() {
    let exporter = exporter_of(0xA0);
    let server = key(1).public_key();
    let login = sign_user_auth(&key(2), &exporter, &server);
    // Claiming someone else's key with one's own signature.
    assert_eq!(
        verify_user_auth(&key(3).public_key(), &exporter, &server, &login),
        Err(InvalidSignature)
    );
}

#[test]
fn user_signature_fails_when_any_bit_is_altered() {
    let exporter = exporter_of(0xA0);
    let (user_key, server) = (key(2), key(1).public_key());
    let user = user_key.public_key();
    let login = sign_user_auth(&user_key, &exporter, &server);
    for bit in 0..512 {
        let altered = Signature::from_bytes(flip_bit(login.as_bytes(), bit));
        assert_eq!(
            verify_user_auth(&user, &exporter, &server, &altered),
            Err(InvalidSignature),
            "signature bit {bit}"
        );
    }
    for bit in 0..256 {
        let altered = Exporter::from_bytes(flip_bit(exporter.as_bytes(), bit));
        assert_eq!(
            verify_user_auth(&user, &altered, &server, &login),
            Err(InvalidSignature),
            "exporter bit {bit}"
        );
    }
}

/// The two signed messages have different labels, so a proof of one kind is
/// never accepted as the other, even when one key plays both roles.
#[test]
fn proofs_of_one_kind_are_not_accepted_as_the_other() {
    let exporter = exporter_of(0xA0);
    let both = key(5);
    let public = both.public_key();

    let server_proof = sign_server_proof(&both, &exporter);
    assert_eq!(
        verify_user_auth(&public, &exporter, &public, &server_proof),
        Err(InvalidSignature)
    );
    let login = sign_user_auth(&both, &exporter, &public);
    assert_eq!(
        verify_server_proof(&public, &exporter, &login),
        Err(InvalidSignature)
    );
    // Nor is a plain signature over the bare exporter.
    let bare = both.sign(exporter.as_bytes());
    assert_eq!(
        verify_server_proof(&public, &exporter, &bare),
        Err(InvalidSignature)
    );
}

#[test]
fn truncated_inputs_are_refused_before_any_verification() {
    let exporter = exporter_of(0xA0);
    let proof = sign_server_proof(&key(1), &exporter);

    assert_eq!(
        Signature::from_slice(&proof.as_bytes()[..63]),
        Err(LengthError {
            expected: 64,
            found: 63
        })
    );
    assert_eq!(
        Exporter::from_slice(&exporter.as_bytes()[..31]),
        Err(LengthError {
            expected: 32,
            found: 31
        })
    );
    assert_eq!(
        Exporter::from_slice(&[0; 33]),
        Err(LengthError {
            expected: 32,
            found: 33
        })
    );
    assert_eq!(Exporter::from_slice(exporter.as_bytes()), Ok(exporter));
    assert_eq!(Exporter::from_slice(&[]).ok(), None);
}

// ---------------------------------------------------------------------------
// password_proof
// ---------------------------------------------------------------------------

fn params_of(case: &serde_json::Value) -> PasswordKdfParams {
    PasswordKdfParams::new(
        number32(case, "memory_kib"),
        number32(case, "iterations"),
        number32(case, "parallelism"),
        &common::bytes(case, "salt"),
    )
    .expect("the vectors use accepted parameters")
}

/// Argon2id and the HMAC, against OpenSSL (`tests/vectors/generate.py`).
#[test]
fn password_proof_matches_the_vectors() {
    let file = vectors("statements.json");
    for case in list(&file, "password") {
        let exporter = exporter(case, "exporter");
        let key = PasswordKey::derive(text(case, "password"), &params_of(case)).expect("Argon2id");
        assert_eq!(to_hex(key.expose()), text(case, "key"));

        let proof = key.proof(&exporter);
        assert_eq!(to_hex(proof.as_bytes()), text(case, "proof"));
        assert_eq!(key.verify(&exporter, proof.as_bytes()), Ok(()));

        // The server holds the stored key, not the password.
        let stored = PasswordKey::from_bytes(&array(case, "key"));
        assert_eq!(stored.verify(&exporter, proof.as_bytes()), Ok(()));
    }
}

#[test]
fn password_proof_fails_on_another_connection() {
    let key = PasswordKey::from_bytes(&[7; 32]);
    let proof = key.proof(&exporter_of(0xA0));
    assert_eq!(
        key.verify(&exporter_of(0xA1), proof.as_bytes()),
        Err(InvalidPasswordProof)
    );
}

#[test]
fn password_proof_fails_for_another_password() {
    let exporter = exporter_of(0xA0);
    let proof = PasswordKey::from_bytes(&[7; 32]).proof(&exporter);
    assert_eq!(
        PasswordKey::from_bytes(&[8; 32]).verify(&exporter, proof.as_bytes()),
        Err(InvalidPasswordProof)
    );
}

#[test]
fn password_proof_fails_when_truncated_extended_or_altered() {
    let exporter = exporter_of(0xA0);
    let key = PasswordKey::from_bytes(&[7; 32]);
    let proof = *key.proof(&exporter).as_bytes();

    for length in [0, 1, 16, 31] {
        assert_eq!(
            key.verify(&exporter, &proof[..length]),
            Err(InvalidPasswordProof),
            "{length} bytes"
        );
    }
    let mut extended = proof.to_vec();
    extended.push(0);
    assert_eq!(key.verify(&exporter, &extended), Err(InvalidPasswordProof));

    for bit in 0..256 {
        assert_eq!(
            key.verify(&exporter, &flip_bit(&proof, bit)),
            Err(InvalidPasswordProof),
            "proof bit {bit}"
        );
    }
    assert_eq!(key.verify(&exporter, &proof), Ok(()));
}

/// A different password, salt or cost gives a different key, so the proof
/// of one does not pass for the other. One Argon2id run per variant.
#[test]
fn password_key_depends_on_password_salt_and_cost() {
    let salt = [0x5A; 16];
    let params = PasswordKdfParams::new(19 * 1024, 2, 1, &salt).expect("minimum parameters");
    let base = PasswordKey::derive("hunter2", &params).expect("Argon2id");

    let other_password = PasswordKey::derive("hunter3", &params).expect("Argon2id");
    let other_salt = PasswordKey::derive(
        "hunter2",
        &PasswordKdfParams::new(19 * 1024, 2, 1, &[0x5B; 16]).expect("parameters"),
    )
    .expect("Argon2id");
    let other_cost = PasswordKey::derive(
        "hunter2",
        &PasswordKdfParams::new(19 * 1024, 3, 1, &salt).expect("parameters"),
    )
    .expect("Argon2id");

    let exporter = exporter_of(0xA0);
    let proof = base.proof(&exporter);
    for (name, other) in [
        ("password", &other_password),
        ("salt", &other_salt),
        ("cost", &other_cost),
    ] {
        assert_ne!(other.expose(), base.expose(), "{name}");
        assert_eq!(
            other.verify(&exporter, proof.as_bytes()),
            Err(InvalidPasswordProof),
            "{name}"
        );
    }

    // The same inputs give the same key.
    let again = PasswordKey::derive("hunter2", &params).expect("Argon2id");
    assert_eq!(again.expose(), base.expose());
}

// ---------------------------------------------------------------------------
// Parameters from an untrusted server
// ---------------------------------------------------------------------------

#[test]
fn kdf_parameters_have_a_floor_and_a_ceiling() {
    let salt = [0u8; 16];
    let new = PasswordKdfParams::new;

    // The floor: a man in the middle must not get a cheap proof to attack.
    assert_eq!(
        new(8, 1, 1, &salt),
        Err(KdfParamsError::Memory { found: 8 })
    );
    assert_eq!(
        new(19 * 1024 - 1, 2, 1, &salt),
        Err(KdfParamsError::Memory {
            found: 19 * 1024 - 1
        })
    );
    assert_eq!(
        new(19 * 1024, 1, 1, &salt),
        Err(KdfParamsError::Iterations { found: 1 })
    );
    assert_eq!(
        new(19 * 1024, 0, 1, &salt),
        Err(KdfParamsError::Iterations { found: 0 })
    );
    assert_eq!(
        new(19 * 1024, 2, 0, &salt),
        Err(KdfParamsError::Parallelism { found: 0 })
    );
    assert_eq!(
        new(19 * 1024, 2, 1, &salt[..15]),
        Err(KdfParamsError::SaltLength { found: 15 })
    );
    assert_eq!(
        new(19 * 1024, 2, 1, &[]),
        Err(KdfParamsError::SaltLength { found: 0 })
    );

    // The ceiling: a hostile server must not exhaust the client.
    assert_eq!(
        new(256 * 1024 + 1, 2, 1, &salt),
        Err(KdfParamsError::Memory {
            found: 256 * 1024 + 1
        })
    );
    assert_eq!(
        new(u32::MAX, 2, 1, &salt),
        Err(KdfParamsError::Memory { found: u32::MAX })
    );
    assert_eq!(
        new(19 * 1024, 17, 1, &salt),
        Err(KdfParamsError::Iterations { found: 17 })
    );
    assert_eq!(
        new(19 * 1024, u32::MAX, 1, &salt),
        Err(KdfParamsError::Iterations { found: u32::MAX })
    );
    assert_eq!(
        new(19 * 1024, 2, 17, &salt),
        Err(KdfParamsError::Parallelism { found: 17 })
    );
    assert_eq!(
        new(19 * 1024, 2, 1, &[0; 65]),
        Err(KdfParamsError::SaltLength { found: 65 })
    );

    // The corners are accepted.
    let low = new(19 * 1024, 2, 1, &salt).expect("the floor");
    assert_eq!(
        (low.memory_kib(), low.iterations(), low.parallelism()),
        (19_456, 2, 1)
    );
    assert_eq!(low.salt(), &salt);
    assert!(new(256 * 1024, 16, 16, &[0; 64]).is_ok());
}

#[test]
fn recommended_parameters_are_rfc_9106_second_choice() {
    let params = PasswordKdfParams::recommended(&[1; 16]).expect("16-byte salt");
    assert_eq!(params.memory_kib(), 64 * 1024);
    assert_eq!(params.iterations(), 3);
    assert_eq!(params.parallelism(), 4);
    assert_eq!(params.salt(), &[1; 16]);
    assert_eq!(
        PasswordKdfParams::recommended(&[1; 8]),
        Err(KdfParamsError::SaltLength { found: 8 })
    );
}
