//! Secrets do not leak through formatting: secret types cannot be printed
//! or cloned at all, sensitive types print no secret part, and errors never
//! quote what they refuse.
//!
//! `docs/rust-rewrite/04-connection-security.md` section 9: "Secret types
//! are zeroed on drop and cannot be formatted for logs."

mod common;

use std::fmt::{Debug, Display};

use common::{key, to_hex};
use parties_identity::handshake::{Exporter, PasswordKdfParams, PasswordKey, PasswordProof};
use parties_identity::{
    ImportCandidate, JoinCode, JoinLink, LinkStatement, PhraseV1, PhraseV2, PublicKey,
    RecoveryPhrase, SigningKey, import_phrase, passkey,
};
use zeroize::ZeroizeOnDrop;

// ---------------------------------------------------------------------------
// Compile-time: what secret types do not implement
// ---------------------------------------------------------------------------

/// Fails to compile if the type implements the trait.
///
/// With the trait implemented, both blanket implementations below apply and
/// the marker type in `AmbiguousIfImpl<_>` cannot be inferred.
macro_rules! assert_not_impl {
    ($type:ty: $($trait:path),+ $(,)?) => {$(
        const _: fn() = || {
            trait AmbiguousIfImpl<Marker> {
                fn some_item() {}
            }
            impl<T: ?Sized> AmbiguousIfImpl<()> for T {}
            #[allow(dead_code)]
            struct Implemented;
            impl<T: ?Sized + $trait> AmbiguousIfImpl<Implemented> for T {}
            let _ = <$type as AmbiguousIfImpl<_>>::some_item;
        };
    )+};
}

/// Fails to compile unless the type implements the traits.
macro_rules! assert_impl {
    ($type:ty: $($trait:path),+ $(,)?) => {
        const _: fn() = || {
            fn requires<T: ?Sized $(+ $trait)+>() {}
            requires::<$type>();
        };
    };
}

// Long-term secrets: no formatting of any kind, no copies.
assert_not_impl!(SigningKey: Debug, Display, Clone, Copy);
assert_not_impl!(PhraseV1: Debug, Display, Clone, Copy);
assert_not_impl!(PhraseV2: Debug, Display, Clone, Copy);
assert_not_impl!(RecoveryPhrase: Debug, Display, Clone, Copy);
assert_not_impl!(PasswordKey: Debug, Display, Clone, Copy);
// They are wiped when dropped.
assert_impl!(SigningKey: ZeroizeOnDrop);
assert_impl!(PhraseV1: ZeroizeOnDrop);
assert_impl!(PhraseV2: ZeroizeOnDrop);
assert_impl!(RecoveryPhrase: ZeroizeOnDrop);
assert_impl!(PasswordKey: ZeroizeOnDrop);
assert_impl!(JoinCode: ZeroizeOnDrop);

// Holds a signing key: a `Debug` that shows public parts only, no `Display`.
assert_not_impl!(ImportCandidate: Display, Clone, Copy);
// Credentials and per-connection values: a redacted `Debug`, no `Display`.
assert_not_impl!(JoinCode: Display, Copy);
assert_not_impl!(JoinLink: Display, Copy);
assert_not_impl!(Exporter: Display);
assert_not_impl!(PasswordProof: Display, PartialEq);

// Public values print and compare freely.
assert_impl!(PublicKey: Debug, Display, Copy, Eq, std::hash::Hash, Ord, Send, Sync);
assert_impl!(SigningKey: Send, Sync);

// ---------------------------------------------------------------------------
// Run time: what `Debug` prints
// ---------------------------------------------------------------------------

const ABOUT: &str = "abandon abandon abandon abandon abandon abandon \
                     abandon abandon abandon abandon abandon about";

/// Every form in which the bytes could show up in a `Debug` output.
fn assert_no_bytes(output: &str, secret: &[u8], what: &str) {
    let lower = to_hex(secret);
    let decimal_list = format!("{secret:?}");
    let inner = decimal_list.trim_matches(|character| character == '[' || character == ']');
    for needle in [lower.as_str(), &lower.to_ascii_uppercase(), inner] {
        assert!(
            !output.contains(needle),
            "{what}: found the bytes in {output:?}"
        );
    }
    // Not even a recognisable piece of them.
    assert!(
        !output.to_ascii_lowercase().contains(&lower[..16]),
        "{what}: found the first bytes in {output:?}"
    );
}

#[test]
fn import_candidates_show_fingerprint_only() {
    let candidates = import_phrase(ABOUT).expect("twelve words");
    assert_eq!(candidates.len(), 2);
    let output = format!("{candidates:?}");
    let pretty = format!("{candidates:#?}");

    for candidate in &candidates {
        let fingerprint = candidate.signing_key.fingerprint().to_string();
        assert!(output.contains(&fingerprint));
        let seed = candidate.signing_key.to_seed();
        for text in [&output, &pretty] {
            assert_no_bytes(text, seed.as_slice(), "seed");
            for word in ["abandon", "about"] {
                assert!(!text.contains(word), "phrase word in {text:?}");
            }
        }
        if let RecoveryPhrase::V2(phrase) = &candidate.phrase {
            assert_no_bytes(&output, phrase.bip39_seed("").as_slice(), "BIP-39 seed");
        }
    }
    assert!(output.contains("V1") && output.contains("V2"));
}

#[test]
fn join_codes_and_links_do_not_show_the_code() {
    let code = JoinCode::parse("7KQ2M-9X01T").expect("code");
    assert_eq!(format!("{code:?}"), "JoinCode(..)");
    assert_eq!(format!("{code:#?}"), "JoinCode(..)");
    assert_eq!(format!("{:?}", Some(&code)), "Some(JoinCode(..))");

    let server = key(7).public_key();
    let text = format!("parties://voice.example.org:7800/j/7KQ2M-9X01T#k={server}");
    let link = JoinLink::parse(&text).expect("link");
    for output in [format!("{link:?}"), format!("{link:#?}")] {
        for piece in ["7KQ2M", "9X01T", "7kq2m", "9x01t", "/j/"] {
            assert!(!output.contains(piece), "{piece} in {output:?}");
        }
        // The public parts are there, so the output is still useful.
        assert!(output.contains("voice.example.org"));
        assert!(output.contains("7800"));
        assert!(output.contains(&server.to_string()));
        assert!(output.contains("JoinCode(..)"));
    }

    let without_code = JoinLink::parse(&format!("parties://example.org:7800#k={server}"));
    assert!(format!("{without_code:?}").contains("join_code: None"));
}

#[test]
fn exporter_and_statements_do_not_show_the_exporter() {
    let bytes: [u8; 32] = std::array::from_fn(|index| 0xA0 + index as u8);
    let exporter = Exporter::from_bytes(bytes);
    assert_eq!(format!("{exporter:?}"), "Exporter(..)");

    let statement = LinkStatement::new(
        &exporter,
        &key(1).public_key(),
        &key(2).public_key(),
        &key(3).public_key(),
    )
    .expect("two different keys");
    let output = format!("{statement:?}");
    assert_no_bytes(&output, &bytes, "exporter");
    assert!(output.contains(&key(3).public_key().to_string()));
}

#[test]
fn password_proof_is_not_shown() {
    let key = PasswordKey::from_bytes(&[0x5A; 32]);
    let proof = key.proof(&Exporter::from_bytes([1; 32]));
    let output = format!("{proof:?}");
    assert_eq!(output, "PasswordProof(..)");
    assert_no_bytes(&output, proof.as_bytes(), "proof");
}

#[test]
fn kdf_parameters_are_public_and_printable() {
    // The salt and the cost travel in the clear in HelloAck; printing them
    // is fine, and useful when a server is misconfigured.
    let params = PasswordKdfParams::recommended(&[7; 16]).expect("parameters");
    let output = format!("{params:?}");
    assert!(output.contains("65536"));
}

// ---------------------------------------------------------------------------
// Run time: errors do not quote their input
// ---------------------------------------------------------------------------

#[test]
fn phrase_errors_do_not_quote_the_phrase() {
    let secret_word = "hunter2hunter2";
    let typed = ABOUT.replace("about", secret_word);
    for error in [
        PhraseV1::parse(&typed).err().expect("refused"),
        PhraseV2::parse(&typed).err().expect("refused"),
        import_phrase(&typed).expect_err("refused"),
        PhraseV2::parse(ABOUT.trim_end_matches("about"))
            .err()
            .expect("refused"),
        PhraseV2::parse(&ABOUT.replace("about", "abandon"))
            .err()
            .expect("refused"),
    ] {
        for output in [error.to_string(), format!("{error:?}")] {
            assert!(!output.contains(secret_word), "{output}");
            assert!(!output.contains("abandon"), "{output}");
        }
    }
}

#[test]
fn key_errors_do_not_quote_the_seed() {
    let seed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7fzz";
    let error = SigningKey::from_seed_hex(seed).err().expect("refused");
    for output in [error.to_string(), format!("{error:?}")] {
        assert!(!output.contains("9d61"), "{output}");
        assert!(!output.contains("7fzz"), "{output}");
    }
}

#[test]
fn link_and_code_errors_do_not_quote_the_code() {
    let code_error = JoinCode::parse("7KQ2M-9XU1T").expect_err("refused");
    let link_errors = [
        JoinLink::parse("parties://example.org:7800/j/7KQ2M-9XU1T#k=short"),
        JoinLink::parse("parties://example.org:7800/j/7KQ2M-9X01T#k=short"),
        JoinLink::parse("parties://example.org:7800/j/7KQ2M-9X01T"),
        JoinLink::parse("parties://exa_mple.org:7800/j/7KQ2M-9X01T#k=short"),
    ];
    let mut outputs = vec![code_error.to_string(), format!("{code_error:?}")];
    for error in link_errors {
        let error = error.expect_err("refused");
        outputs.push(error.to_string());
        outputs.push(format!("{error:?}"));
    }
    for output in outputs {
        for piece in ["7KQ2M", "9XU1T", "9X01T"] {
            assert!(!output.contains(piece), "{piece} in {output}");
        }
    }
}

// ---------------------------------------------------------------------------
// Getting a secret out is always a deliberate call
// ---------------------------------------------------------------------------

#[test]
fn secrets_come_out_only_through_named_calls() {
    let key = SigningKey::from_seed(&[7; 32]);
    assert_eq!(key.to_seed().as_slice(), &[7; 32]);
    assert_eq!(key.to_seed_hex().as_str(), "07".repeat(32));

    let phrase = PhraseV2::from_entropy(&[0; 16]).expect("entropy");
    assert_eq!(phrase.expose().as_str(), ABOUT);
    assert_eq!(phrase.entropy(), [0; 16]);

    let old = PhraseV1::parse(ABOUT).expect("phrase");
    assert_eq!(old.expose(), ABOUT);

    let password_key = PasswordKey::from_bytes(&[9; 32]);
    assert_eq!(password_key.expose(), &[9; 32]);

    let (from_passkey, _) = passkey::unlock(&[0; 32], None).expect("no record");
    assert_eq!(from_passkey.word_count(), 12);
}
