//! Fingerprints: the colon-separated hex of the C++ client and the six
//! words of a server identity.

mod common;

use common::{array, key, list, public_key, text, vectors};
use parties_identity::{Fingerprint, FingerprintError, LengthError, SixWords, wordlist};

#[test]
fn fingerprints_match_the_vectors() {
    let file = vectors("identity.json");
    for case in list(&file, "keys") {
        let public = public_key(case, "public_key");
        let expected = text(case, "fingerprint");
        let fingerprint = public.fingerprint();

        assert_eq!(fingerprint, Fingerprint::of(&public));
        assert_eq!(fingerprint.to_string(), expected);
        assert_eq!(expected.len(), 95);
        assert_eq!(expected.parse::<Fingerprint>(), Ok(fingerprint));
        assert_eq!(
            format!("{fingerprint:?}"),
            format!("Fingerprint({expected})")
        );
    }
}

/// The six-word encoding is fixed: changing it would change what users of
/// every server compare. This vector pins it, in addition to the file.
#[test]
fn six_words_of_a_known_key_are_stable() {
    let fingerprint = key(7).fingerprint();
    assert_eq!(
        fingerprint.to_string(),
        "fe:81:2c:12:f3:ab:4c:e6:ac:5d:b6:9a:c3:52:f9:06:\
         cb:1b:11:ef:43:fb:33:e2:52:ef:7f:f5:52:26:38:89"
    );
    let words = fingerprint.words();
    assert_eq!(words.to_string(), "write annual again trap regular infant");
    assert_eq!(
        words.words(),
        ["write", "annual", "again", "trap", "regular", "infant"]
    );
    // fe 81 2c 12 f3 ab 4c e6 ac: the first 66 bits in groups of 11.
    assert_eq!(words.indices(), [2036, 75, 37, 1850, 1446, 922]);
    assert_eq!(
        format!("{words:?}"),
        "SixWords(write annual again trap regular infant)"
    );
}

#[test]
fn six_words_match_the_vectors() {
    let file = vectors("identity.json");
    for case in list(&file, "keys") {
        let fingerprint = public_key(case, "public_key").fingerprint();
        let expected = text(case, "six_words");

        assert_eq!(fingerprint.words().to_string(), expected);
        let parsed: SixWords = expected.parse().expect("six words");
        assert!(parsed.matches(&fingerprint));
        assert_eq!(parsed, fingerprint.words());
    }
}

#[test]
fn six_words_cover_exactly_the_first_66_bits() {
    let mut bytes = [0u8; 32];
    let all_zero = Fingerprint::from_bytes(bytes).words();
    assert_eq!(all_zero.words(), ["abandon"; 6]);

    // Bit 65, the last one that counts, is the second-highest bit of byte 8.
    bytes[8] = 0b0100_0000;
    let last_bit = Fingerprint::from_bytes(bytes).words();
    assert_eq!(last_bit.indices(), [0, 0, 0, 0, 0, 1]);

    // Bit 66 and everything after it do not change the words.
    bytes = [0u8; 32];
    bytes[8] = 0b0011_1111;
    bytes[9..].fill(0xFF);
    assert_eq!(Fingerprint::from_bytes(bytes).words(), all_zero);

    let all_one = Fingerprint::from_bytes([0xFF; 32]).words();
    assert_eq!(all_one.words(), ["zoo"; 6]);
    assert_eq!(all_one.indices(), [2047; 6]);
}

#[test]
fn six_words_are_read_in_any_case_and_with_hyphens() {
    let fingerprint = key(7).fingerprint();
    for typed in [
        "write annual again trap regular infant",
        "WRITE ANNUAL AGAIN TRAP REGULAR INFANT",
        "Write-Annual-Again-Trap-Regular-Infant",
        "  write\tannual\nagain   trap - regular -infant  ",
    ] {
        let words: SixWords = typed.parse().expect(typed);
        assert!(words.matches(&fingerprint), "{typed:?}");
    }

    let other: SixWords = "write annual again trap regular indoor"
        .parse()
        .expect("six words");
    assert!(!other.matches(&fingerprint));
}

#[test]
fn six_words_refuse_wrong_counts_and_unknown_words() {
    let parse = |text: &str| text.parse::<SixWords>();
    assert_eq!(parse(""), Err(FingerprintError::WordCount { found: 0 }));
    assert_eq!(
        parse("write annual again trap regular"),
        Err(FingerprintError::WordCount { found: 5 })
    );
    assert_eq!(
        parse("write annual again trap regular infant zoo"),
        Err(FingerprintError::WordCount { found: 7 })
    );
    assert_eq!(
        parse("write annual agian trap regular infant"),
        Err(FingerprintError::UnknownWord { position: 3 })
    );
    assert_eq!(
        parse("write,annual,again,trap,regular,infant"),
        Err(FingerprintError::UnknownWord { position: 1 })
    );
}

#[test]
fn fingerprint_text_is_read_in_either_case() {
    let fingerprint = key(7).fingerprint();
    let lower = fingerprint.to_string();
    assert_eq!(
        lower.to_ascii_uppercase().parse::<Fingerprint>(),
        Ok(fingerprint)
    );
    assert_eq!(lower, lower.to_ascii_lowercase());
}

#[test]
fn fingerprint_text_refuses_other_shapes() {
    let good = key(7).fingerprint().to_string();
    let parse = |text: &str| text.parse::<Fingerprint>();
    let length = |found| {
        Err(FingerprintError::Length(LengthError {
            expected: 95,
            found,
        }))
    };

    assert_eq!(parse(""), length(0));
    assert_eq!(parse(&good[..94]), length(94));
    assert_eq!(parse(&format!("{good}:")), length(96));
    // Plain hex without colons, as some tools print SHA-256.
    assert_eq!(parse(&good.replace(':', "")), length(64));

    let mut wrong_separator = good.clone();
    wrong_separator.replace_range(2..3, "-");
    assert_eq!(parse(&wrong_separator), Err(FingerprintError::Encoding));

    let mut wrong_digit = good.clone();
    wrong_digit.replace_range(0..1, "g");
    assert_eq!(parse(&wrong_digit), Err(FingerprintError::Encoding));

    // A colon where a digit belongs.
    let shifted = format!(":{}", &good[..94]);
    assert_eq!(parse(&shifted), Err(FingerprintError::Encoding));

    // 95 bytes, but not 95 characters.
    let multibyte = format!("{}\u{e9}", &good[..93]);
    assert_eq!(multibyte.len(), 95);
    assert_eq!(parse(&multibyte), Err(FingerprintError::Encoding));
}

#[test]
fn fingerprint_bytes_round_trip() {
    let file = vectors("identity.json");
    let case = &list(&file, "keys")[0];
    let fingerprint = public_key(case, "public_key").fingerprint();
    assert_eq!(
        Fingerprint::from_bytes(*fingerprint.as_bytes()),
        fingerprint
    );

    // The fingerprint is the SHA-256 of the key: the hex of the vector,
    // without the colons, is that digest.
    let digest: [u8; 32] = common::hex(&text(case, "fingerprint").replace(':', ""))
        .try_into()
        .expect("32 bytes");
    assert_eq!(fingerprint.as_bytes(), &digest);
    let _: [u8; 32] = array(case, "public_key");
}

#[test]
fn word_list_lookup_is_public_and_consistent() {
    assert_eq!(wordlist::WORD_COUNT, 2048);
    assert_eq!(wordlist::word(0), Some("abandon"));
    assert_eq!(wordlist::word(2047), Some("zoo"));
    assert_eq!(wordlist::word(2048), None);
    assert_eq!(wordlist::index_of("Zoo"), Some(2047));
    assert_eq!(wordlist::index_of("zoos"), None);
    assert_eq!(wordlist::ENGLISH.len(), 2048);
}
