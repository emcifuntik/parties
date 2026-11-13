//! Property tests: text forms round-trip, parsers survive any input, and
//! every verify function accepts what its sign function produced and
//! nothing that is one bit off.

mod common;

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};

use common::{config, flip_bit, flip_bit_vec};
use parties_identity::handshake::{
    Exporter, InvalidPasswordProof, PasswordKey, sign_server_proof, sign_user_auth,
    verify_server_proof, verify_user_auth,
};
use parties_identity::{
    Fingerprint, Host, ImportReading, InvalidSignature, JoinCode, JoinLink, LevelSearch,
    LinkKeyError, LinkStatement, PhraseV1, PhraseV2, PinCheck, PublicKey, RotationStatement,
    Signature, SigningKey, SixWords, check_pin, identity_level, import_phrase,
    search_identity_level, verify_identity_level, wordlist,
};
use proptest::collection::vec;
use proptest::prelude::*;
use proptest::sample::select;

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------

/// The seed of a signing key.
fn seed() -> impl Strategy<Value = [u8; 32]> {
    any::<[u8; 32]>()
}

fn exporter() -> impl Strategy<Value = Exporter> {
    any::<[u8; 32]>().prop_map(Exporter::from_bytes)
}

/// Entropy of one of the five BIP-39 lengths.
fn entropy() -> impl Strategy<Value = Vec<u8>> {
    select(vec![16usize, 20, 24, 28, 32]).prop_flat_map(|length| vec(any::<u8>(), length))
}

/// Twelve words of the list, as the C++ client would generate them.
fn twelve_words() -> impl Strategy<Value = String> {
    vec(0u16..2048, 12).prop_map(|indices| {
        indices
            .into_iter()
            .map(|index| wordlist::word(index).expect("index below 2048"))
            .collect::<Vec<_>>()
            .join(" ")
    })
}

fn host() -> impl Strategy<Value = Host> {
    prop_oneof![
        "[a-z][a-z0-9]{0,12}(\\.[a-z]([a-z0-9-]{0,8}[a-z0-9])?){0,3}"
            .prop_map(|name| Host::parse(&name).expect("a well-formed name")),
        // Addresses a link refuses (unspecified, multicast, broadcast) are skipped.
        any::<[u8; 4]>().prop_filter_map("an address of one host", |octets| {
            Host::from_ip(IpAddr::V4(Ipv4Addr::from(octets))).ok()
        }),
        any::<[u16; 8]>().prop_filter_map("an address of one host", |segments| {
            Host::from_ip(IpAddr::V6(Ipv6Addr::from(segments))).ok()
        }),
    ]
}

fn join_code() -> impl Strategy<Value = JoinCode> {
    any::<[u8; 10]>().prop_map(|random| JoinCode::from_random(&random))
}

fn join_link() -> impl Strategy<Value = JoinLink> {
    (
        host(),
        1u16..=u16::MAX,
        proptest::option::of(join_code()),
        seed(),
    )
        .prop_map(|(host, port, code, seed)| {
            let server = SigningKey::from_seed(&seed).public_key();
            JoinLink::new(host, port, code, server).expect("a port above zero")
        })
}

// ---------------------------------------------------------------------------
// Keys and signatures
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(96))]

    /// Verify accepts what sign produced, and nothing one bit off: not in
    /// the signature, not in the message, not under another key.
    #[test]
    fn signature_verifies_and_nothing_one_bit_off_does(
        seed in seed(),
        other_seed in seed(),
        message in vec(any::<u8>(), 0..300),
        signature_bit in 0usize..512,
        message_bit in any::<usize>(),
    ) {
        let key = SigningKey::from_seed(&seed);
        let public = key.public_key();
        let signature = key.sign(&message);
        prop_assert_eq!(public.verify(&message, &signature), Ok(()));

        let altered = Signature::from_bytes(flip_bit(signature.as_bytes(), signature_bit));
        prop_assert_eq!(public.verify(&message, &altered), Err(InvalidSignature));

        if !message.is_empty() {
            let altered = flip_bit_vec(&message, message_bit);
            prop_assert_eq!(public.verify(&altered, &signature), Err(InvalidSignature));
        }
        let mut longer = message.clone();
        longer.push(0);
        prop_assert_eq!(public.verify(&longer, &signature), Err(InvalidSignature));

        let other = SigningKey::from_seed(&other_seed).public_key();
        if other != public {
            prop_assert_eq!(other.verify(&message, &signature), Err(InvalidSignature));
        }
    }

    /// Every key made from a seed is accepted as a public key, in bytes and
    /// in text, and comes back unchanged.
    #[test]
    fn public_key_forms_round_trip(seed in seed()) {
        let key = SigningKey::from_seed(&seed);
        let public = key.public_key();
        prop_assert_eq!(PublicKey::from_bytes(public.as_bytes()), Ok(public));
        prop_assert_eq!(PublicKey::from_slice(public.as_bytes()), Ok(public));
        let text = public.to_string();
        prop_assert_eq!(text.len(), 43);
        prop_assert_eq!(text.parse::<PublicKey>(), Ok(public));
        prop_assert_eq!(*key.to_seed(), seed);
        let again = SigningKey::from_seed_hex(&key.to_seed_hex()).expect("its own hex");
        prop_assert_eq!(again.public_key(), public);
    }

    /// Arbitrary bytes are a key or an error, never a panic, and an
    /// accepted key has exactly the bytes it was made from.
    #[test]
    fn arbitrary_bytes_are_a_key_or_an_error(bytes in any::<[u8; 32]>()) {
        if let Ok(key) = PublicKey::from_bytes(&bytes) {
            prop_assert_eq!(key.to_bytes(), bytes);
            prop_assert_eq!(key.to_string().parse::<PublicKey>(), Ok(key));
        }
    }

    /// Arbitrary text is a key or an error, and an accepted text is the
    /// canonical one.
    #[test]
    fn arbitrary_text_is_a_key_or_an_error(
        text in prop_oneof!["\\PC{0,64}", "[A-Za-z0-9_-]{43}", "[A-Za-z0-9_-]{42}[AQgw]"],
    ) {
        if let Ok(key) = text.parse::<PublicKey>() {
            prop_assert_eq!(key.to_string(), text);
        }
    }

    #[test]
    fn fingerprint_text_round_trips(bytes in any::<[u8; 32]>()) {
        let fingerprint = Fingerprint::from_bytes(bytes);
        let text = fingerprint.to_string();
        prop_assert_eq!(text.len(), 95);
        prop_assert_eq!(text.parse::<Fingerprint>(), Ok(fingerprint));
        prop_assert_eq!(text.to_ascii_uppercase().parse::<Fingerprint>(), Ok(fingerprint));

        let words = fingerprint.words();
        prop_assert_eq!(words.to_string().parse::<SixWords>(), Ok(words));
        prop_assert!(words.matches(&fingerprint));
        prop_assert_eq!(words.words().join("-").to_uppercase().parse::<SixWords>(), Ok(words));
    }

    /// Fingerprints that differ in their first 66 bits have different words.
    #[test]
    fn six_words_tell_fingerprints_apart(bytes in any::<[u8; 32]>(), bit in 0usize..66) {
        let mut other = bytes;
        other[bit / 8] ^= 0x80 >> (bit % 8);
        let words = Fingerprint::from_bytes(bytes).words();
        prop_assert_ne!(Fingerprint::from_bytes(other).words(), words);
        prop_assert!(!words.matches(&Fingerprint::from_bytes(other)));
    }

    #[test]
    fn arbitrary_text_is_a_fingerprint_or_an_error(
        text in prop_oneof!["\\PC{0,100}", "([0-9a-fA-F]{2}:){31}[0-9a-fA-F]{2}"],
    ) {
        if let Ok(fingerprint) = text.parse::<Fingerprint>() {
            prop_assert_eq!(fingerprint.to_string(), text.to_ascii_lowercase());
        }
        let _ = text.parse::<SixWords>();
    }
}

// ---------------------------------------------------------------------------
// Recovery phrases
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(64))]

    /// Entropy to words and back, however the words are typed.
    #[test]
    fn bip39_round_trips(entropy in entropy(), upper in any::<bool>(), separator in "[ \\t\\n]{1,3}") {
        let phrase = PhraseV2::from_entropy(&entropy).expect("a BIP-39 length");
        prop_assert_eq!(phrase.entropy(), entropy.as_slice());
        prop_assert_eq!(phrase.word_count(), entropy.len() / 4 * 3);

        let text = phrase.expose();
        prop_assert_eq!(text.split(' ').count(), phrase.word_count());
        let parsed = PhraseV2::parse(&text).expect("the phrase it wrote");
        prop_assert_eq!(parsed.entropy(), entropy.as_slice());

        let mut typed = text.replace(' ', &separator);
        if upper {
            typed = typed.to_ascii_uppercase();
        }
        let parsed = PhraseV2::parse(&typed).expect("the same words");
        prop_assert_eq!(parsed.entropy(), entropy.as_slice());
    }

    /// Replacing one word by another never yields the same phrase: it is a
    /// checksum error or other entropy.
    #[test]
    fn bip39_notices_a_replaced_word(
        entropy in entropy(),
        position in any::<prop::sample::Index>(),
        replacement in 0u16..2048,
    ) {
        let phrase = PhraseV2::from_entropy(&entropy).expect("a BIP-39 length");
        let text = phrase.expose();
        let mut words: Vec<&str> = text.split(' ').collect();
        let position = position.index(words.len());
        let replacement = wordlist::word(replacement).expect("index below 2048");
        prop_assume!(words[position] != replacement);
        words[position] = replacement;

        if let Ok(other) = PhraseV2::parse(&words.join(" ")) {
            prop_assert_ne!(other.entropy(), entropy.as_slice());
        }
    }

    /// Any text is a phrase or an error, never a panic.
    #[test]
    fn arbitrary_text_is_a_phrase_or_an_error(
        text in prop_oneof![
            "\\PC{0,200}",
            "([a-z]{3,8}[ \\t\\n]{1,2}){0,30}",
            "(abandon|zoo|about|Zoo|wrong|abuot)( (abandon|zoo|about|wrong)){0,26}",
        ],
    ) {
        let _ = PhraseV1::parse(&text);
        let _ = PhraseV2::parse(&text);
        if let Ok(candidates) = import_phrase(&text) {
            prop_assert!((1..=3).contains(&candidates.len()));
        }
    }
}

proptest! {
    // Each case runs PBKDF2 a few times.
    #![proptest_config(config(24))]

    /// What import offers is exactly what the two versions accept, and each
    /// candidate carries the key its phrase derives.
    #[test]
    fn import_agrees_with_both_versions(text in twelve_words(), trailing in "[ \\n]{0,1}") {
        let typed = format!("{text}{trailing}");
        let candidates = import_phrase(&typed).expect("twelve words are always importable");
        let readings: Vec<ImportReading> =
            candidates.iter().map(|candidate| candidate.reading).collect();

        let mut expected = Vec::new();
        if PhraseV2::parse(&typed).is_ok() {
            expected.push(ImportReading::V2);
        }
        expected.push(ImportReading::V1);
        if !trailing.is_empty() {
            expected.push(ImportReading::V1AsTyped);
        }
        prop_assert_eq!(readings, expected);

        for candidate in &candidates {
            let public = candidate.signing_key.public_key();
            prop_assert_eq!(candidate.phrase.signing_key().public_key(), public);
            prop_assert_eq!(candidate.phrase.version(), candidate.reading.version());
            let direct = match candidate.reading {
                ImportReading::V2 => PhraseV2::parse(&typed).expect("valid").signing_key(),
                ImportReading::V1 => PhraseV1::parse(&text).expect("valid").signing_key(),
                ImportReading::V1AsTyped => PhraseV1::parse(&typed).expect("valid").signing_key(),
            };
            prop_assert_eq!(direct.public_key(), public);
        }
    }

    /// Different entropy, different identity; the same entropy, the same.
    #[test]
    fn v2_identity_is_a_function_of_the_entropy(first in entropy(), second in entropy()) {
        let key = |entropy: &[u8]| {
            PhraseV2::from_entropy(entropy).expect("a BIP-39 length").signing_key().public_key()
        };
        prop_assert_eq!(key(&first), key(&first));
        if first != second {
            prop_assert_ne!(key(&first), key(&second));
        }
    }
}

// ---------------------------------------------------------------------------
// Join codes and links
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(256))]

    #[test]
    fn join_code_forms_round_trip(random in any::<[u8; 10]>()) {
        let code = JoinCode::from_random(&random);
        let shown = code.expose();
        prop_assert_eq!(shown.len(), 11);
        prop_assert_eq!(shown.as_bytes()[5], b'-');
        prop_assert_eq!(JoinCode::parse(&shown), Ok(code.clone()));
        prop_assert_eq!(JoinCode::parse(&shown.to_ascii_lowercase()), Ok(code.clone()));
        let compact = code.expose_compact();
        prop_assert_eq!(compact.len(), 10);
        prop_assert_eq!(JoinCode::parse(&compact), Ok(code.clone()));
        prop_assert_eq!(shown.replace('-', ""), compact.as_str());
    }

    /// Any text is a code or an error; an accepted text reads the same after
    /// it is written in the canonical form.
    #[test]
    fn arbitrary_text_is_a_join_code_or_an_error(
        text in prop_oneof!["\\PC{0,16}", "[0-9A-Za-z]{10}", "[0-9A-Za-z]{5}-[0-9A-Za-z]{5}"],
    ) {
        if let Ok(code) = JoinCode::parse(&text) {
            prop_assert_eq!(JoinCode::parse(&code.expose()), Ok(code));
        }
    }

    /// Format then parse gives the link back; parse then format gives the
    /// text back.
    #[test]
    fn join_link_round_trips(link in join_link()) {
        let text = link.to_uri();
        let parsed = JoinLink::parse(&text);
        prop_assert_eq!(parsed.as_ref(), Ok(&link));
        prop_assert_eq!(parsed.expect("checked above").to_uri(), text);
    }

    /// A valid link with one character replaced is a link or an error, never
    /// a panic; if it is still a link, it is a consistent one.
    #[test]
    fn damaged_join_links_do_not_panic(
        link in join_link(),
        position in any::<prop::sample::Index>(),
        replacement in any::<char>(),
        cut in any::<prop::sample::Index>(),
    ) {
        let text = link.to_uri();
        let mut characters: Vec<char> = text.chars().collect();
        let at = position.index(characters.len());
        characters[at] = replacement;
        let damaged: String = characters.iter().collect();
        if let Ok(parsed) = JoinLink::parse(&damaged) {
            prop_assert_eq!(JoinLink::parse(&parsed.to_uri()), Ok(parsed));
        }

        // Truncated anywhere: never the same link with a different key or
        // code, and never a panic.
        let truncated: String = text.chars().take(cut.index(text.len())).collect();
        if let Ok(parsed) = JoinLink::parse(&truncated) {
            prop_assert_eq!(parsed.server_key(), link.server_key());
        }
    }

    #[test]
    fn arbitrary_text_is_a_join_link_or_an_error(
        text in prop_oneof![
            "\\PC{0,300}",
            "parties://[a-z0-9.:\\[\\]-]{0,40}(/j?/?[0-9A-Za-z-]{0,12})?(#[k=&A-Za-z0-9_-]{0,60})?",
        ],
    ) {
        if let Ok(link) = JoinLink::parse(&text) {
            prop_assert_eq!(JoinLink::parse(&link.to_uri()), Ok(link));
        }
        let _ = Host::parse(&text);
    }
}

// ---------------------------------------------------------------------------
// Handshake proofs, rotation, key links
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(64))]

    #[test]
    fn server_proof_is_bound_to_key_and_connection(
        seed in seed(),
        exporter in exporter(),
        exporter_bit in 0usize..256,
        signature_bit in 0usize..512,
    ) {
        let server_key = SigningKey::from_seed(&seed);
        let server = server_key.public_key();
        let proof = sign_server_proof(&server_key, &exporter);
        prop_assert_eq!(verify_server_proof(&server, &exporter, &proof), Ok(()));

        let other_connection = Exporter::from_bytes(flip_bit(exporter.as_bytes(), exporter_bit));
        prop_assert_eq!(
            verify_server_proof(&server, &other_connection, &proof),
            Err(InvalidSignature)
        );
        let altered = Signature::from_bytes(flip_bit(proof.as_bytes(), signature_bit));
        prop_assert_eq!(verify_server_proof(&server, &exporter, &altered), Err(InvalidSignature));
    }

    #[test]
    fn user_signature_is_bound_to_user_server_and_connection(
        user_seed in seed(),
        server_seed in seed(),
        other_seed in seed(),
        exporter in exporter(),
        exporter_bit in 0usize..256,
        signature_bit in 0usize..512,
    ) {
        let user_key = SigningKey::from_seed(&user_seed);
        let user = user_key.public_key();
        let server = SigningKey::from_seed(&server_seed).public_key();
        let other = SigningKey::from_seed(&other_seed).public_key();
        let login = sign_user_auth(&user_key, &exporter, &server);
        prop_assert_eq!(verify_user_auth(&user, &exporter, &server, &login), Ok(()));

        let other_connection = Exporter::from_bytes(flip_bit(exporter.as_bytes(), exporter_bit));
        prop_assert_eq!(
            verify_user_auth(&user, &other_connection, &server, &login),
            Err(InvalidSignature)
        );
        let altered = Signature::from_bytes(flip_bit(login.as_bytes(), signature_bit));
        prop_assert_eq!(
            verify_user_auth(&user, &exporter, &server, &altered),
            Err(InvalidSignature)
        );
        if other != server {
            prop_assert_eq!(
                verify_user_auth(&user, &exporter, &other, &login),
                Err(InvalidSignature)
            );
        }
        if other != user {
            prop_assert_eq!(
                verify_user_auth(&other, &exporter, &server, &login),
                Err(InvalidSignature)
            );
        }
    }

    #[test]
    fn password_proof_is_bound_to_key_and_connection(
        key in any::<[u8; 32]>(),
        exporter in exporter(),
        key_bit in 0usize..256,
        exporter_bit in 0usize..256,
        proof_bit in 0usize..256,
        length in 0usize..32,
    ) {
        let password_key = PasswordKey::from_bytes(&key);
        let proof = *password_key.proof(&exporter).as_bytes();
        prop_assert_eq!(password_key.verify(&exporter, &proof), Ok(()));

        let invalid = Err(InvalidPasswordProof);
        prop_assert_eq!(password_key.verify(&exporter, &flip_bit(&proof, proof_bit)), invalid);
        prop_assert_eq!(password_key.verify(&exporter, &proof[..length]), invalid);
        let other_connection = Exporter::from_bytes(flip_bit(exporter.as_bytes(), exporter_bit));
        prop_assert_eq!(password_key.verify(&other_connection, &proof), invalid);
        let other_key = PasswordKey::from_bytes(&flip_bit(&key, key_bit));
        prop_assert_eq!(other_key.verify(&exporter, &proof), invalid);
    }

    #[test]
    fn rotation_statement_round_trips_and_nothing_one_bit_off_does(
        old_seed in seed(),
        new_seed in seed(),
        bit in 0usize..1024,
    ) {
        prop_assume!(old_seed != new_seed);
        let old_key = SigningKey::from_seed(&old_seed);
        let new_public = SigningKey::from_seed(&new_seed).public_key();
        let statement = RotationStatement::sign(&old_key, &new_public).expect("different keys");

        let encoded = statement.to_bytes();
        prop_assert_eq!(RotationStatement::from_bytes(&encoded), Ok(statement));
        prop_assert!(RotationStatement::from_bytes(&flip_bit(&encoded, bit)).is_err());
        prop_assert!(RotationStatement::from_bytes(&encoded[..bit % 128]).is_err());

        let old_public = old_key.public_key();
        prop_assert_eq!(check_pin(Some(&old_public), &new_public, &[statement]), PinCheck::Rotated);
        prop_assert_eq!(check_pin(Some(&new_public), &old_public, &[statement]), PinCheck::Mismatch);
        prop_assert_eq!(check_pin(Some(&old_public), &new_public, &[]), PinCheck::Mismatch);
        prop_assert_eq!(check_pin(None, &new_public, &[statement]), PinCheck::Unknown);
    }

    #[test]
    fn link_proof_is_bound_to_every_field(
        seeds in any::<[[u8; 32]; 4]>(),
        exporter in exporter(),
        exporter_bit in 0usize..256,
        signature_bit in 0usize..512,
    ) {
        let [server_seed, account_seed, new_seed, other_seed] = seeds;
        let server = SigningKey::from_seed(&server_seed).public_key();
        let account = SigningKey::from_seed(&account_seed).public_key();
        let new_key = SigningKey::from_seed(&new_seed);
        let new_public = new_key.public_key();
        let other = SigningKey::from_seed(&other_seed).public_key();
        prop_assume!(account != new_public && other != new_public && other != account);
        prop_assume!(other != server);

        let statement =
            LinkStatement::new(&exporter, &server, &account, &new_public).expect("different keys");
        let proof = statement.sign_with_new_key(&new_key).expect("the new key");
        prop_assert_eq!(statement.verify_new_key(&proof), Ok(()));

        let invalid = Err(LinkKeyError::Signature(InvalidSignature));
        let altered = Signature::from_bytes(flip_bit(proof.as_bytes(), signature_bit));
        prop_assert_eq!(statement.verify_new_key(&altered), invalid);

        let other_connection = Exporter::from_bytes(flip_bit(exporter.as_bytes(), exporter_bit));
        let variants = [
            LinkStatement::new(&other_connection, &server, &account, &new_public),
            LinkStatement::new(&exporter, &other, &account, &new_public),
            LinkStatement::new(&exporter, &server, &other, &new_public),
        ];
        for variant in variants {
            let variant = variant.expect("different keys");
            prop_assert_eq!(variant.verify_new_key(&proof), invalid);
        }
    }
}

// ---------------------------------------------------------------------------
// Identity level
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(64))]

    /// The search returns the first counter of its range that reaches the
    /// target, or the best of the range, exactly as a plain loop would.
    #[test]
    fn level_search_agrees_with_a_plain_loop(
        seed in seed(),
        target in 0u32..10,
        start in prop_oneof![0u64..1_000_000, (u64::MAX - 300)..=u64::MAX],
        attempts in 0u64..200,
    ) {
        let key = SigningKey::from_seed(&seed).public_key();

        let mut expected_best: Option<(u64, u32)> = None;
        let mut expected_found = None;
        let mut tried_all = false;
        let mut counter = start;
        for _ in 0..attempts {
            let level = identity_level(&key, counter);
            if level >= target {
                expected_found = Some((counter, level));
                break;
            }
            if expected_best.is_none_or(|(_, best)| level > best) {
                expected_best = Some((counter, level));
            }
            match counter.checked_add(1) {
                Some(next) => counter = next,
                None => {
                    tried_all = true;
                    break;
                }
            }
        }

        let result = search_identity_level(&key, target, start, attempts);
        match expected_found {
            Some((counter, level)) => {
                prop_assert_eq!(result, LevelSearch::Found { counter, level });
                prop_assert_eq!(verify_identity_level(&key, counter, target), Ok(level));
            }
            None => {
                let resume_at = if tried_all { None } else { Some(counter) };
                prop_assert_eq!(
                    result,
                    LevelSearch::Exhausted { best: expected_best, resume_at }
                );
            }
        }
    }

    /// The check accepts a counter exactly when its level reaches the
    /// minimum.
    #[test]
    fn level_check_agrees_with_the_level(
        seed in seed(),
        counter in any::<u64>(),
        minimum in 0u32..12,
    ) {
        let key = SigningKey::from_seed(&seed).public_key();
        let level = identity_level(&key, counter);
        prop_assert!(level <= 256);
        let checked = verify_identity_level(&key, counter, minimum);
        prop_assert_eq!(checked.is_ok(), level >= minimum);
        match checked {
            Ok(found) => prop_assert_eq!(found, level),
            Err(error) => {
                prop_assert_eq!(error.found, level);
                prop_assert_eq!(error.required, minimum);
            }
        }
    }
}
