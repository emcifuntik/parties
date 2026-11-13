//! Ed25519 keys and signatures: the RFC 8032 vectors, what verification
//! refuses, which bytes are a public key, and the text forms.

mod common;

use common::{
    array, bytes, flip_bit, hex, key, list, public_key, signature, signing_key, text, to_hex,
    vectors,
};
use parties_identity::{InvalidSignature, KeyError, LengthError, PublicKey, Signature, SigningKey};

// ---------------------------------------------------------------------------
// RFC 8032 section 7.1
// ---------------------------------------------------------------------------

#[test]
fn rfc8032_vectors_sign_and_verify() {
    let file = vectors("rfc8032.json");
    let cases = list(&file, "cases");
    assert!(cases.len() >= 4);
    for case in cases {
        let name = text(case, "name");
        let key = signing_key(case, "seed");
        let message = bytes(case, "message");
        let expected = signature(case, "signature");

        assert_eq!(key.public_key(), public_key(case, "public_key"), "{name}");
        assert_eq!(key.sign(&message), expected, "{name}");
        assert_eq!(
            key.public_key().verify(&message, &expected),
            Ok(()),
            "{name}"
        );
    }
}

#[test]
fn signing_is_deterministic() {
    let key = key(1);
    assert_eq!(key.sign(b"message"), key.sign(b"message"));
    assert_ne!(key.sign(b"message"), key.sign(b"massage"));
}

// ---------------------------------------------------------------------------
// What verification refuses
// ---------------------------------------------------------------------------

#[test]
fn verify_refuses_another_message() {
    let key = key(1);
    let signature = key.sign(b"message");
    for message in [&b"massage"[..], b"", b"message ", b"messag"] {
        assert_eq!(
            key.public_key().verify(message, &signature),
            Err(InvalidSignature)
        );
    }
}

#[test]
fn verify_refuses_another_key() {
    let signature = key(1).sign(b"message");
    assert_eq!(
        key(2).public_key().verify(b"message", &signature),
        Err(InvalidSignature)
    );
}

#[test]
fn verify_refuses_every_single_bit_change_of_a_signature() {
    let file = vectors("rfc8032.json");
    for (index, case) in list(&file, "cases").iter().enumerate() {
        let key = public_key(case, "public_key");
        let message = bytes(case, "message");
        let good: [u8; 64] = array(case, "signature");
        // Every bit for the first vector, every seventh for the others.
        let step = if index == 0 { 1 } else { 7 };
        for bit in (0..512).step_by(step) {
            let altered = Signature::from_bytes(flip_bit(&good, bit));
            assert_eq!(
                key.verify(&message, &altered),
                Err(InvalidSignature),
                "bit {bit}"
            );
        }
    }
}

#[test]
fn verify_refuses_garbage_signatures() {
    let key = key(1).public_key();
    for bytes in [[0u8; 64], [0xFF; 64], [0x55; 64]] {
        assert_eq!(
            key.verify(b"message", &Signature::from_bytes(bytes)),
            Err(InvalidSignature)
        );
    }
}

/// The order of the base point, little-endian.
const GROUP_ORDER: [u8; 32] = [
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
];

/// `S + L` is the classic second form of a signature: the equation still
/// holds, so a verifier that does not check `S < L` accepts it.
#[test]
fn verify_refuses_a_signature_with_s_plus_group_order() {
    let file = vectors("rfc8032.json");
    let case = &list(&file, "cases")[0];
    let key = public_key(case, "public_key");
    let message = bytes(case, "message");
    let mut malleated: [u8; 64] = array(case, "signature");

    let mut carry = 0u16;
    for (byte, order) in malleated[32..].iter_mut().zip(GROUP_ORDER) {
        let sum = u16::from(*byte) + u16::from(order) + carry;
        *byte = (sum & 0xFF) as u8;
        carry = sum >> 8;
    }
    assert_eq!(carry, 0, "S + L still fits 32 bytes for this vector");

    assert_eq!(
        key.verify(&message, &Signature::from_bytes(malleated)),
        Err(InvalidSignature)
    );
}

// ---------------------------------------------------------------------------
// Which bytes are a public key
// ---------------------------------------------------------------------------

/// The eight points of small order, in their canonical encodings.
const SMALL_ORDER: [&str; 8] = [
    "0100000000000000000000000000000000000000000000000000000000000000",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000000000000000000000000000000000080",
    "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",
    "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
    "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
    "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
];

/// Encodings a lenient decoder maps to a point, but that are not the one
/// encoding of that point: `y` not reduced, or the sign bit set on `x = 0`.
/// The last two decode to points of large order.
const NON_CANONICAL: [&str; 8] = [
    "0100000000000000000000000000000000000000000000000000000000000080",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "f0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "f1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
];

/// `y` values with no matching `x`.
const NOT_ON_CURVE: [&str; 3] = [
    "0200000000000000000000000000000000000000000000000000000000000000",
    "0700000000000000000000000000000000000000000000000000000000000000",
    "0800000000000000000000000000000000000000000000000000000000000000",
];

fn key_bytes(text: &str) -> [u8; 32] {
    hex(text).try_into().expect("32 bytes")
}

#[test]
fn small_order_keys_are_refused() {
    for encoding in SMALL_ORDER {
        assert_eq!(
            PublicKey::from_bytes(&key_bytes(encoding)),
            Err(KeyError::WeakKey),
            "{encoding}"
        );
    }
}

#[test]
fn non_canonical_encodings_are_refused() {
    for encoding in NON_CANONICAL {
        assert_eq!(
            PublicKey::from_bytes(&key_bytes(encoding)),
            Err(KeyError::InvalidPoint),
            "{encoding}"
        );
    }
}

#[test]
fn bytes_that_are_no_point_are_refused() {
    for encoding in NOT_ON_CURVE {
        assert_eq!(
            PublicKey::from_bytes(&key_bytes(encoding)),
            Err(KeyError::InvalidPoint),
            "{encoding}"
        );
    }
}

#[test]
fn keys_of_the_wrong_length_are_refused() {
    let good = key(1).public_key();
    for length in [0, 1, 31, 33, 64] {
        let bytes = vec![0x11; length];
        assert_eq!(
            PublicKey::from_slice(&bytes),
            Err(KeyError::Length(LengthError {
                expected: 32,
                found: length
            }))
        );
    }
    assert_eq!(PublicKey::from_slice(good.as_bytes()), Ok(good));
    assert_eq!(PublicKey::from_bytes(&good.to_bytes()), Ok(good));
}

#[test]
fn signatures_of_the_wrong_length_are_refused() {
    let good = key(1).sign(b"message");
    for length in [0, 63, 65, 128] {
        let bytes = vec![0x11; length];
        assert_eq!(
            Signature::from_slice(&bytes),
            Err(LengthError {
                expected: 64,
                found: length
            })
        );
    }
    assert_eq!(Signature::from_slice(good.as_bytes()), Ok(good));
    // A truncated signature cannot even be built, let alone verified.
    assert!(Signature::from_slice(&good.as_bytes()[..63]).is_err());
}

// ---------------------------------------------------------------------------
// Text forms
// ---------------------------------------------------------------------------

#[test]
fn public_key_text_matches_the_vectors() {
    let file = vectors("identity.json");
    for case in list(&file, "keys") {
        let key = signing_key(case, "seed");
        let public = key.public_key();
        let expected = text(case, "public_key_text");

        assert_eq!(public, public_key(case, "public_key"));
        assert_eq!(public.to_string(), expected);
        assert_eq!(expected.len(), 43);
        assert_eq!(expected.parse::<PublicKey>(), Ok(public));
        assert_eq!(format!("{public:?}"), format!("PublicKey({expected})"));
    }
}

#[test]
fn public_key_text_has_one_form() {
    let public = key(7).public_key();
    let good = public.to_string();
    assert_eq!(good, "6kpsY-KcUgq-9VB7Ey7F-ZVHdq6-vnuSQh7qaRRG0iw");

    let length = |found| {
        Err(KeyError::Length(LengthError {
            expected: 43,
            found,
        }))
    };
    // Padding, white space, a missing or an extra character.
    assert_eq!(format!("{good}=").parse::<PublicKey>(), length(44));
    assert_eq!(format!(" {good}").parse::<PublicKey>(), length(44));
    assert_eq!(good[..42].parse::<PublicKey>(), length(42));
    assert_eq!("".parse::<PublicKey>(), length(0));

    // The standard alphabet instead of the URL-safe one.
    let standard = good.replace('-', "+");
    assert_eq!(standard.parse::<PublicKey>(), Err(KeyError::Encoding));
    // The last character carries four bits; the two unused ones must be zero.
    let unused_bits = format!("{}x", &good[..42]);
    assert_eq!(unused_bits.parse::<PublicKey>(), Err(KeyError::Encoding));
    // Characters outside the alphabet, also ones that take several bytes.
    assert_eq!(
        format!("{}!", &good[..42]).parse::<PublicKey>(),
        Err(KeyError::Encoding)
    );
    let multibyte = format!("{}\u{e9}", &good[..41]);
    assert_eq!(multibyte.len(), 43);
    assert_eq!(multibyte.parse::<PublicKey>(), Err(KeyError::Encoding));

    // Valid base64url of bytes that are not a usable key: the identity
    // point (01 00 .. 00) and a `y` that is not on the curve (02 00 .. 00).
    let rest = "A".repeat(41);
    assert_eq!(
        format!("AQ{rest}").parse::<PublicKey>(),
        Err(KeyError::WeakKey)
    );
    assert_eq!(
        format!("Ag{rest}").parse::<PublicKey>(),
        Err(KeyError::InvalidPoint)
    );
}

#[test]
fn seed_hex_round_trips_and_is_the_1x_private_key_form() {
    let seed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
    let key = SigningKey::from_seed_hex(seed).expect("64 hex digits");
    assert_eq!(key.to_seed_hex().as_str(), seed);
    assert_eq!(to_hex(key.to_seed().as_slice()), seed);
    assert_eq!(
        to_hex(key.public_key().as_bytes()),
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    );

    // The C++ client accepts uppercase digits too.
    let upper = SigningKey::from_seed_hex(&seed.to_ascii_uppercase()).expect("uppercase hex");
    assert_eq!(upper.public_key(), key.public_key());
}

#[test]
fn seed_hex_refuses_wrong_length_and_wrong_digits() {
    let seed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
    let error = |text: &str| SigningKey::from_seed_hex(text).err();
    let length = |found| {
        Some(KeyError::Length(LengthError {
            expected: 64,
            found,
        }))
    };

    assert_eq!(error(&seed[..63]), length(63));
    assert_eq!(error(&format!("{seed}0")), length(65));
    assert_eq!(error(""), length(0));
    assert_eq!(
        error(&format!("{}g", &seed[..63])),
        Some(KeyError::Encoding)
    );
    assert_eq!(
        error(&format!("{} ", &seed[..63])),
        Some(KeyError::Encoding)
    );
    assert_eq!(
        error(&format!("0x{}", &seed[..62])),
        Some(KeyError::Encoding)
    );
}

/// The answers of the C++ client itself (`tests/vectors/cpp_v1.json`, made
/// by running `secret_key_from_hex` and `derive_pubkey` of
/// `common/src/crypto.cpp`): the same texts are a private key, and each
/// gives the same public key.
#[test]
fn seed_hex_agrees_with_the_cpp_program() {
    let file = vectors("cpp_v1.json");
    let (mut accepted, mut refused) = (0, 0);
    for case in list(&file, "hex_keys") {
        let input = text(case, "input");
        let valid = common::member(case, "valid")
            .as_bool()
            .expect("'valid' must be a boolean");
        let typed = String::from_utf8(bytes(case, "input")).expect("the inputs are text");

        match SigningKey::from_seed_hex(&typed) {
            Ok(key) => {
                assert!(valid, "{input}: the C++ refuses this");
                assert_eq!(key.public_key(), public_key(case, "public_key"), "{input}");
                accepted += 1;
            }
            Err(_) => {
                assert!(!valid, "{input}: the C++ accepts this");
                refused += 1;
            }
        }
    }
    assert!(accepted >= 20 && refused >= 15, "{accepted} / {refused}");
}

#[test]
fn signature_debug_is_hex() {
    let signature = Signature::from_bytes([0xAB; 64]);
    assert_eq!(
        format!("{signature:?}"),
        format!("Signature({})", "ab".repeat(64))
    );
    assert_eq!(signature.to_bytes(), [0xAB; 64]);
}

#[cfg(feature = "os-rng")]
#[test]
fn generated_keys_differ_and_work() {
    let first = SigningKey::generate();
    let second = SigningKey::generate();
    assert_ne!(first.public_key(), second.public_key());

    let signature = first.sign(b"message");
    assert_eq!(first.public_key().verify(b"message", &signature), Ok(()));
    assert_eq!(
        second.public_key().verify(b"message", &signature),
        Err(InvalidSignature)
    );
}
