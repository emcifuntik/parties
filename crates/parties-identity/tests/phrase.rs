//! Recovery phrases: the official BIP-39 vectors, the version 2 derivation,
//! bit-for-bit compatibility of version 1 with the C++ client, and import.

mod common;

use common::{array, bytes, list, public_key, text, to_hex, vectors};
use parties_identity::phrase::{
    BIP39_ROUNDS, BIP39_SEED_LEN, PHRASE_V2_INFO, RECOVERY_ENTROPY_LEN, V1_WORD_COUNT,
};
use parties_identity::{
    ImportReading, PhraseError, PhraseV1, PhraseV2, PhraseVersion, RecoveryPhrase, import_phrase,
};
use serde_json::Value;

const ABOUT: &str = "abandon abandon abandon abandon abandon abandon \
                     abandon abandon abandon abandon abandon about";

/// Twelve words without a valid BIP-39 checksum.
const ABANDON: &str = "abandon abandon abandon abandon abandon abandon \
                       abandon abandon abandon abandon abandon abandon";

/// The version 1 vector with the given name.
fn v1_case<'a>(file: &'a Value, name: &str) -> &'a Value {
    list(file, "v1")
        .iter()
        .find(|case| text(case, "name") == name)
        .unwrap_or_else(|| panic!("no v1 vector named '{name}'"))
}

/// The version 2 vector with the given phrase.
fn v2_case<'a>(file: &'a Value, phrase: &str) -> &'a Value {
    list(file, "v2")
        .iter()
        .find(|case| text(case, "phrase") == phrase)
        .unwrap_or_else(|| panic!("no v2 vector for that phrase"))
}

// ---------------------------------------------------------------------------
// Constants that are part of the derivation
// ---------------------------------------------------------------------------

#[test]
fn derivation_constants_are_pinned() {
    assert_eq!(PHRASE_V2_INFO, b"parties/phrase/identity/v2");
    assert_eq!(BIP39_ROUNDS, 2048);
    assert_eq!(BIP39_SEED_LEN, 64);
    assert_eq!(RECOVERY_ENTROPY_LEN, 16);
    assert_eq!(V1_WORD_COUNT, 12);
    assert_eq!(PhraseVersion::V1.number(), 1);
    assert_eq!(PhraseVersion::V2.number(), 2);
    for version in [PhraseVersion::V1, PhraseVersion::V2] {
        assert_eq!(PhraseVersion::from_number(version.number()), Some(version));
    }
    assert_eq!(PhraseVersion::from_number(0), None);
    assert_eq!(PhraseVersion::from_number(3), None);
}

// ---------------------------------------------------------------------------
// BIP-39: the official vectors (passphrase "TREZOR")
// ---------------------------------------------------------------------------

#[test]
fn official_bip39_vectors() {
    let file = vectors("bip39.json");
    let passphrase = text(&file, "passphrase");
    assert_eq!(passphrase, "TREZOR");
    let cases = list(&file, "cases");
    assert!(cases.len() >= 4);

    for case in cases {
        let entropy = bytes(case, "entropy");
        let phrase_text = text(case, "phrase");
        let seed: [u8; 64] = array(case, "seed");

        let phrase = PhraseV2::from_entropy(&entropy).expect("valid entropy length");
        assert_eq!(phrase.expose().as_str(), phrase_text);
        assert_eq!(phrase.entropy(), entropy);
        assert_eq!(phrase.word_count(), phrase_text.split(' ').count());
        assert_eq!(*phrase.bip39_seed(passphrase), seed, "{phrase_text}");

        let parsed = PhraseV2::parse(phrase_text).expect("valid phrase");
        assert_eq!(parsed.entropy(), entropy);
        assert_eq!(*parsed.bip39_seed(passphrase), seed);
    }
}

#[test]
fn bip39_covers_every_standard_length() {
    for (length, words) in [(16, 12), (20, 15), (24, 18), (28, 21), (32, 24)] {
        let entropy: Vec<u8> = (0..length).map(|byte| byte as u8 ^ 0xA5).collect();
        let phrase = PhraseV2::from_entropy(&entropy).expect("valid entropy length");
        assert_eq!(phrase.word_count(), words);
        let text = phrase.expose();
        assert_eq!(text.split(' ').count(), words);
        let parsed = PhraseV2::parse(&text).expect("the phrase it wrote");
        assert_eq!(parsed.entropy(), entropy);
    }
}

#[test]
fn entropy_of_other_lengths_is_refused() {
    for length in [0, 1, 15, 17, 18, 31, 33, 64] {
        let entropy = vec![0u8; length];
        assert_eq!(
            PhraseV2::from_entropy(&entropy).err(),
            Some(PhraseError::EntropyLength { found: length })
        );
    }
}

// ---------------------------------------------------------------------------
// Version 2: BIP-39 seed with the empty passphrase, then HKDF
// ---------------------------------------------------------------------------

#[test]
fn v2_derivation_matches_the_vectors() {
    let file = vectors("identity.json");
    for case in list(&file, "v2") {
        let phrase_text = text(case, "phrase");
        let phrase = PhraseV2::from_entropy(&bytes(case, "entropy")).expect("entropy");
        assert_eq!(phrase.expose().as_str(), phrase_text);

        let bip39_seed: [u8; 64] = array(case, "bip39_seed");
        assert_eq!(*phrase.bip39_seed(""), bip39_seed, "{phrase_text}");

        let key = phrase.signing_key();
        assert_eq!(to_hex(key.to_seed().as_slice()), text(case, "seed"));
        assert_eq!(key.public_key(), public_key(case, "public_key"));
        assert_eq!(key.fingerprint().to_string(), text(case, "fingerprint"));

        // The same through the text and through the enum.
        let parsed = PhraseV2::parse(phrase_text).expect("phrase");
        assert_eq!(parsed.signing_key().public_key(), key.public_key());
        let either = RecoveryPhrase::from(parsed);
        assert_eq!(either.version(), PhraseVersion::V2);
        assert_eq!(either.expose().as_str(), phrase_text);
        assert_eq!(either.signing_key().public_key(), key.public_key());
    }
}

#[test]
fn v2_reading_ignores_case_and_spacing() {
    let phrase = PhraseV2::parse(ABOUT).expect("phrase");
    for typed in [
        ABOUT.to_ascii_uppercase(),
        format!("  {ABOUT}  "),
        ABOUT.replace(' ', "\n"),
        ABOUT.replace(' ', " \t "),
        ABOUT.replace(' ', "\u{a0}"),
        ABOUT.replace("about", "About"),
    ] {
        let again = PhraseV2::parse(&typed).expect("same phrase, typed differently");
        assert_eq!(again.entropy(), phrase.entropy(), "{typed:?}");
        assert_eq!(again.expose().as_str(), ABOUT);
    }
}

#[test]
fn v2_reading_refuses_wrong_phrases() {
    let error = |text: &str| PhraseV2::parse(text).err();

    assert_eq!(error(ABANDON), Some(PhraseError::Checksum));
    // Two words swapped: every word is valid, the checksum is not.
    let swapped = "winner legal thank year wave sausage worth useful legal winner thank yellow";
    assert_eq!(error(swapped), Some(PhraseError::Checksum));

    assert_eq!(error(""), Some(PhraseError::WordCount { found: 0 }));
    assert_eq!(error("   "), Some(PhraseError::WordCount { found: 0 }));
    assert_eq!(error("abandon"), Some(PhraseError::WordCount { found: 1 }));
    let eleven = ABOUT.rsplit_once(' ').expect("words").0;
    assert_eq!(error(eleven), Some(PhraseError::WordCount { found: 11 }));
    let thirteen = format!("{ABOUT} abandon");
    assert_eq!(error(&thirteen), Some(PhraseError::WordCount { found: 13 }));
    let many = ["abandon"; 25].join(" ");
    assert_eq!(error(&many), Some(PhraseError::WordCount { found: 25 }));
    let very_many = ["abandon"; 1000].join(" ");
    assert_eq!(
        error(&very_many),
        Some(PhraseError::WordCount { found: 1000 })
    );

    let misspelt = ABOUT.replace("about", "abuot");
    assert_eq!(
        error(&misspelt),
        Some(PhraseError::UnknownWord { position: 12 })
    );
    let comma = ABOUT.replacen(' ', ", ", 1);
    assert_eq!(
        error(&comma),
        Some(PhraseError::UnknownWord { position: 1 })
    );
    assert_eq!(
        error("12345"),
        Some(PhraseError::UnknownWord { position: 1 })
    );
}

#[cfg(feature = "os-rng")]
#[test]
fn generated_phrases_have_twelve_words_and_differ() {
    let first = PhraseV2::generate();
    let second = PhraseV2::generate();
    assert_eq!(first.word_count(), 12);
    assert_eq!(first.entropy().len(), RECOVERY_ENTROPY_LEN);
    assert_ne!(first.entropy(), second.entropy());

    let text = first.expose();
    let parsed = PhraseV2::parse(&text).expect("a generated phrase has a valid checksum");
    assert_eq!(parsed.entropy(), first.entropy());
    assert_eq!(
        parsed.signing_key().public_key(),
        first.signing_key().public_key()
    );
}

// ---------------------------------------------------------------------------
// Version 1: bit for bit what the C++ client derives
// ---------------------------------------------------------------------------

/// `derive_keypair` and `public_key_fingerprint` of `common/src/crypto.cpp`,
/// recomputed with OpenSSL by `tests/vectors/generate.py`: the phrase bytes
/// are hashed exactly as given.
#[test]
fn v1_derivation_matches_the_cpp_client() {
    let file = vectors("identity.json");
    let cases = list(&file, "v1");
    assert!(cases.len() >= 10);
    for case in cases {
        let name = text(case, "name");
        let phrase = PhraseV1::parse(text(case, "phrase"))
            .unwrap_or_else(|error| panic!("{name}: the C++ client accepts this: {error}"));
        assert_eq!(phrase.expose(), text(case, "phrase"), "{name}");

        let key = phrase.signing_key();
        assert_eq!(
            to_hex(key.to_seed().as_slice()),
            text(case, "seed"),
            "{name}"
        );
        assert_eq!(key.public_key(), public_key(case, "public_key"), "{name}");
        assert_eq!(
            key.fingerprint().to_string(),
            text(case, "fingerprint"),
            "{name}"
        );

        let either = RecoveryPhrase::from(phrase);
        assert_eq!(either.version(), PhraseVersion::V1);
        assert_eq!(either.expose().as_str(), text(case, "phrase"));
        assert_eq!(either.signing_key().public_key(), key.public_key());
    }
}

/// The answers of the C++ client itself (`tests/vectors/cpp_v1.json`, made
/// by running `validate_seed_phrase`, `derive_keypair` and
/// `public_key_fingerprint` of `common/src/crypto.cpp`): the crate accepts
/// exactly the texts the C++ accepts, derives the same key from each, and
/// import offers every identity the C++ client could have made.
#[test]
fn v1_agrees_with_the_cpp_program() {
    let file = vectors("cpp_v1.json");
    let cases = list(&file, "phrases");
    let (mut accepted, mut refused) = (0, 0);
    for case in cases {
        let input = text(case, "input");
        let valid = common::member(case, "valid")
            .as_bool()
            .expect("'valid' must be a boolean");
        // The C++ takes bytes, the crate takes text. Bytes that are not
        // UTF-8 cannot reach the crate, and the C++ refuses them all.
        let Ok(typed) = String::from_utf8(bytes(case, "input")) else {
            assert!(!valid, "{input}: the C++ accepted bytes that are no text");
            refused += 1;
            continue;
        };

        let phrase = PhraseV1::parse(&typed);
        assert_eq!(phrase.is_ok(), valid, "{input}");
        let Ok(phrase) = phrase else {
            refused += 1;
            // Import never offers the text as typed when the C++ refuses it.
            let as_typed = import_phrase(&typed).is_ok_and(|candidates| {
                candidates
                    .iter()
                    .any(|candidate| candidate.reading == ImportReading::V1AsTyped)
            });
            assert!(!as_typed, "{input}");
            continue;
        };
        accepted += 1;

        let key = phrase.signing_key();
        assert_eq!(to_hex(key.to_seed().as_slice()), text(case, "seed"));
        assert_eq!(key.public_key(), public_key(case, "public_key"), "{input}");
        let fingerprint = key.fingerprint();
        assert_eq!(fingerprint.to_string(), text(case, "fingerprint"));

        let candidates = import_phrase(&typed).expect("the C++ accepted this");
        let offered = candidates.iter().find(|candidate| {
            candidate.reading.version() == PhraseVersion::V1
                && candidate.fingerprint() == fingerprint
        });
        let offered = offered.unwrap_or_else(|| panic!("{input}: not offered by import"));
        let expected = if phrase.is_tidy() {
            ImportReading::V1
        } else {
            ImportReading::V1AsTyped
        };
        assert_eq!(offered.reading, expected, "{input}");
    }
    assert!(accepted >= 30 && refused >= 30, "{accepted} / {refused}");
}

/// Finding F12: the same words with different white space are different
/// identities in 1.x. Version 1 must reproduce that, not repair it.
#[test]
fn v1_white_space_changes_the_identity() {
    let file = vectors("identity.json");
    let tidy = text(v1_case(&file, "integration test client A"), "fingerprint");
    let mut seen = vec![tidy.to_owned()];
    for name in [
        "trailing space",
        "leading space",
        "two spaces after the first word",
        "tab after the first word",
        "trailing line feed",
        "trailing carriage return and line feed",
        "vertical tab and form feed",
    ] {
        let case = v1_case(&file, name);
        let phrase = PhraseV1::parse(text(case, "phrase")).expect(name);
        assert!(!phrase.is_tidy(), "{name}");
        let fingerprint = phrase.signing_key().fingerprint().to_string();
        assert!(!seen.contains(&fingerprint), "{name}");
        seen.push(fingerprint);
    }
    assert!(PhraseV1::parse(ABOUT).expect("phrase").is_tidy());
}

#[test]
fn v1_accepts_what_the_cpp_client_accepts() {
    let accepts = |text: &str| PhraseV1::parse(text).is_ok();

    // No checksum: any twelve words of the list.
    assert!(accepts(ABANDON));
    assert!(accepts(ABOUT));
    assert!(accepts("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo"));
    // Any run of the six "C" locale white-space characters separates words.
    assert!(accepts(&ABOUT.replace(' ', "\t")));
    assert!(accepts(&ABOUT.replace(' ', "\n")));
    assert!(accepts(&ABOUT.replace(' ', "\r\n")));
    assert!(accepts(&ABOUT.replace(' ', "\x0B")));
    assert!(accepts(&ABOUT.replace(' ', "\x0C")));
    assert!(accepts(&format!("\n\n {ABOUT} \t\r\n")));
}

#[test]
fn v1_refuses_what_the_cpp_client_refuses() {
    let error = |text: &str| PhraseV1::parse(text).err();

    assert_eq!(error(""), Some(PhraseError::WordCount { found: 0 }));
    let eleven = ABOUT.rsplit_once(' ').expect("words").0;
    assert_eq!(error(eleven), Some(PhraseError::WordCount { found: 11 }));
    let thirteen = format!("{ABOUT} abandon");
    assert_eq!(error(&thirteen), Some(PhraseError::WordCount { found: 13 }));
    // A valid 24-word BIP-39 phrase is not a 1.x phrase.
    let twenty_four = PhraseV2::from_entropy(&[0; 32]).expect("entropy").expose();
    assert_eq!(
        error(&twenty_four),
        Some(PhraseError::WordCount { found: 24 })
    );

    // The comparison with the list is exact: lowercase only.
    assert_eq!(
        error(&ABOUT.replace("about", "About")),
        Some(PhraseError::UnknownWord { position: 12 })
    );
    assert_eq!(
        error(&ABOUT.to_ascii_uppercase()),
        Some(PhraseError::UnknownWord { position: 1 })
    );
    assert_eq!(
        error(&ABOUT.replace("about", "abuot")),
        Some(PhraseError::UnknownWord { position: 12 })
    );
    // A no-break space is not white space in the "C" locale, so it glues two
    // words into one unknown word.
    assert_eq!(
        error(&ABOUT.replacen(' ', "\u{a0}", 1)),
        Some(PhraseError::UnknownWord { position: 1 })
    );
    // An unknown word is reported even when the count is wrong as well.
    assert_eq!(
        error("abandon xyz"),
        Some(PhraseError::UnknownWord { position: 2 })
    );
}

// ---------------------------------------------------------------------------
// Import: the version is not known
// ---------------------------------------------------------------------------

fn readings(text: &str) -> Vec<ImportReading> {
    import_phrase(text)
        .expect("importable")
        .iter()
        .map(|candidate| candidate.reading)
        .collect()
}

#[test]
fn import_of_a_phrase_without_checksum_is_v1_only() {
    let file = vectors("identity.json");
    let candidates = import_phrase(ABANDON).expect("twelve words");
    assert_eq!(candidates.len(), 1);
    let candidate = &candidates[0];
    assert_eq!(candidate.reading, ImportReading::V1);
    assert_eq!(candidate.reading.version(), PhraseVersion::V1);
    assert_eq!(candidate.phrase.version(), PhraseVersion::V1);
    assert_eq!(candidate.phrase.expose().as_str(), ABANDON);
    assert_eq!(
        candidate.signing_key.fingerprint().to_string(),
        text(v1_case(&file, "no valid BIP-39 checksum"), "fingerprint")
    );
}

#[test]
fn import_of_a_phrase_with_checksum_offers_both_versions() {
    let file = vectors("identity.json");
    let candidates = import_phrase(ABOUT).expect("twelve words");
    assert_eq!(candidates.len(), 2);

    let v2 = &candidates[0];
    assert_eq!(v2.reading, ImportReading::V2);
    assert_eq!(v2.reading.version(), PhraseVersion::V2);
    assert_eq!(v2.phrase.version(), PhraseVersion::V2);
    assert_eq!(
        v2.signing_key.fingerprint().to_string(),
        text(v2_case(&file, ABOUT), "fingerprint")
    );

    let v1 = &candidates[1];
    assert_eq!(v1.reading, ImportReading::V1);
    assert_eq!(v1.phrase.version(), PhraseVersion::V1);
    assert_eq!(
        v1.signing_key.fingerprint().to_string(),
        text(v1_case(&file, "integration test client A"), "fingerprint")
    );

    assert_ne!(v1.signing_key.public_key(), v2.signing_key.public_key());
    for candidate in &candidates {
        // The phrase a candidate carries derives the key it carries.
        assert_eq!(
            candidate.phrase.signing_key().public_key(),
            candidate.signing_key.public_key()
        );
        assert_eq!(candidate.phrase.expose().as_str(), ABOUT);
    }
}

#[test]
fn import_offers_the_text_as_typed_when_1x_would_have_hashed_it() {
    let file = vectors("identity.json");
    let typed = format!("{ABOUT} ");
    let candidates = import_phrase(&typed).expect("twelve words");
    assert_eq!(
        candidates
            .iter()
            .map(|candidate| candidate.reading)
            .collect::<Vec<_>>(),
        [
            ImportReading::V2,
            ImportReading::V1,
            ImportReading::V1AsTyped
        ]
    );

    let tidy = &candidates[1];
    assert_eq!(tidy.phrase.expose().as_str(), ABOUT);
    assert_eq!(
        tidy.signing_key.fingerprint().to_string(),
        text(v1_case(&file, "integration test client A"), "fingerprint")
    );

    let as_typed = &candidates[2];
    assert_eq!(as_typed.reading.version(), PhraseVersion::V1);
    assert_eq!(as_typed.phrase.expose().as_str(), typed);
    assert_eq!(
        as_typed.signing_key.fingerprint().to_string(),
        text(v1_case(&file, "trailing space"), "fingerprint")
    );

    // Every white-space vector comes back as its own candidate.
    for name in [
        "leading space",
        "two spaces after the first word",
        "tab after the first word",
        "trailing line feed",
        "trailing carriage return and line feed",
        "vertical tab and form feed",
    ] {
        let case = v1_case(&file, name);
        let candidates = import_phrase(text(case, "phrase")).expect(name);
        let as_typed = candidates
            .iter()
            .find(|candidate| candidate.reading == ImportReading::V1AsTyped)
            .unwrap_or_else(|| panic!("{name}: no as-typed candidate"));
        assert_eq!(
            as_typed.signing_key.fingerprint().to_string(),
            text(case, "fingerprint"),
            "{name}"
        );
    }
}

#[test]
fn import_does_not_offer_text_that_1x_would_have_refused() {
    // Uppercase and a no-break space: readable, but 1.x never hashed these.
    assert_eq!(
        readings(&ABOUT.to_ascii_uppercase()),
        [ImportReading::V2, ImportReading::V1]
    );
    assert_eq!(
        readings(&ABOUT.replacen(' ', "\u{a0}", 1)),
        [ImportReading::V2, ImportReading::V1]
    );
    assert_eq!(
        readings(&format!("{} ", ABANDON.replacen('a', "A", 1))),
        [ImportReading::V1]
    );
    // Without a checksum, white space alone gives two version 1 readings.
    assert_eq!(
        readings(&format!("{ABANDON}\n")),
        [ImportReading::V1, ImportReading::V1AsTyped]
    );
}

#[test]
fn import_of_longer_phrases_is_v2_only() {
    for length in [20, 24, 28, 32] {
        let phrase = PhraseV2::from_entropy(&vec![0x3C; length]).expect("entropy");
        let text = phrase.expose();
        let candidates = import_phrase(&text).expect("valid phrase");
        assert_eq!(candidates.len(), 1);
        assert_eq!(candidates[0].reading, ImportReading::V2);
        assert_eq!(
            candidates[0].signing_key.public_key(),
            phrase.signing_key().public_key()
        );
    }
}

#[test]
fn import_refuses_what_no_version_accepts() {
    let error = |text: &str| import_phrase(text).err();

    assert_eq!(error(""), Some(PhraseError::WordCount { found: 0 }));
    assert_eq!(error("abandon"), Some(PhraseError::WordCount { found: 1 }));
    let thirteen = format!("{ABOUT} abandon");
    assert_eq!(error(&thirteen), Some(PhraseError::WordCount { found: 13 }));
    assert_eq!(
        error(&ABOUT.replace("about", "abuot")),
        Some(PhraseError::UnknownWord { position: 12 })
    );
    // Fifteen and twenty-four words can only be BIP-39, so a wrong checksum
    // leaves nothing.
    let fifteen = ["abandon"; 15].join(" ");
    assert_eq!(error(&fifteen), Some(PhraseError::Checksum));
    let twenty_four = ["zoo"; 24].join(" ");
    assert_eq!(error(&twenty_four), Some(PhraseError::Checksum));
}

#[test]
fn import_candidates_can_be_picked_by_fingerprint() {
    let file = vectors("identity.json");
    let expected = text(v1_case(&file, "integration test client A"), "fingerprint");
    let candidates = import_phrase(ABOUT).expect("twelve words");
    let chosen = candidates
        .into_iter()
        .find(|candidate| candidate.signing_key.fingerprint().to_string() == expected)
        .expect("the 1.x identity is among the candidates");
    assert_eq!(chosen.reading, ImportReading::V1);
}
