//! Helpers shared by the integration tests: reading the golden vectors in
//! `tests/vectors/`, hex, bit flips and the `proptest` configuration.
//!
//! Where the vectors come from:
//!
//! | File | Source |
//! |---|---|
//! | `rfc8032.json`, `bip39.json` | The published vectors |
//! | `identity.json`, `statements.json` | `generate.py`: Python `hashlib` and OpenSSL, sharing no code with the crate |
//! | `cpp_v1.json` | `generate_cpp.py`: the C++ client's own functions, run by `cpp_reference.cpp` |
//!
//! The readers below are strict: a missing key or a value of the wrong shape
//! fails the test instead of being skipped.

// Every test binary uses a different part of this module.
#![allow(dead_code)]

use std::fs;
use std::path::Path;

use parties_identity::handshake::Exporter;
use parties_identity::{PublicKey, Signature, SigningKey};
use proptest::prelude::*;
use proptest::test_runner::FileFailurePersistence;
use serde_json::Value;

/// Reads one vector file.
pub fn vectors(file: &str) -> Value {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(file);
    let text = fs::read_to_string(&path).unwrap_or_else(|error| panic!("{file}: {error}"));
    let value: Value =
        serde_json::from_str(&text).unwrap_or_else(|error| panic!("{file}: {error}"));
    assert!(
        value.get("about").is_some_and(Value::is_string),
        "{file}: every vector file says where its values come from"
    );
    value
}

/// A member of an object.
pub fn member<'a>(value: &'a Value, key: &str) -> &'a Value {
    value
        .get(key)
        .unwrap_or_else(|| panic!("missing key '{key}'"))
}

/// A list member of an object. An empty list is a broken vector file.
pub fn list<'a>(value: &'a Value, key: &str) -> &'a [Value] {
    let items = member(value, key)
        .as_array()
        .unwrap_or_else(|| panic!("'{key}' must be a list"));
    assert!(!items.is_empty(), "'{key}' must not be empty");
    items
}

/// A string member of an object.
pub fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    member(value, key)
        .as_str()
        .unwrap_or_else(|| panic!("'{key}' must be a string"))
}

/// A non-negative integer member of an object.
pub fn number(value: &Value, key: &str) -> u64 {
    member(value, key)
        .as_u64()
        .unwrap_or_else(|| panic!("'{key}' must be a non-negative integer"))
}

/// A non-negative integer member that fits 32 bits.
pub fn number32(value: &Value, key: &str) -> u32 {
    u32::try_from(number(value, key)).unwrap_or_else(|_| panic!("'{key}' must fit 32 bits"))
}

/// Decodes hex.
pub fn hex(text: &str) -> Vec<u8> {
    assert!(text.len().is_multiple_of(2), "odd number of hex digits");
    (0..text.len())
        .step_by(2)
        .map(|at| u8::from_str_radix(&text[at..at + 2], 16).expect("hex digits"))
        .collect()
}

/// Encodes lowercase hex.
pub fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// A hex member of an object.
pub fn bytes(value: &Value, key: &str) -> Vec<u8> {
    hex(text(value, key))
}

/// A hex member of an object, of exactly `N` bytes.
pub fn array<const N: usize>(value: &Value, key: &str) -> [u8; N] {
    bytes(value, key)
        .try_into()
        .unwrap_or_else(|_| panic!("'{key}' must be {N} bytes"))
}

/// The signing key of a hex seed member.
pub fn signing_key(value: &Value, key: &str) -> SigningKey {
    SigningKey::from_seed(&array(value, key))
}

/// A hex public-key member.
pub fn public_key(value: &Value, key: &str) -> PublicKey {
    PublicKey::from_bytes(&array(value, key)).unwrap_or_else(|error| panic!("'{key}': {error}"))
}

/// A hex signature member.
pub fn signature(value: &Value, key: &str) -> Signature {
    Signature::from_bytes(array(value, key))
}

/// A hex exporter member.
pub fn exporter(value: &Value, key: &str) -> Exporter {
    Exporter::from_bytes(array(value, key))
}

/// The signing key whose seed is 32 times the given byte.
pub fn key(byte: u8) -> SigningKey {
    SigningKey::from_seed(&[byte; 32])
}

/// A copy with one bit flipped. `bit` counts from the first byte's lowest
/// bit and wraps around.
pub fn flip_bit<const N: usize>(bytes: &[u8; N], bit: usize) -> [u8; N] {
    let mut flipped = *bytes;
    flipped[(bit / 8) % N] ^= 1 << (bit % 8);
    flipped
}

/// A copy of a byte vector with one bit flipped.
pub fn flip_bit_vec(bytes: &[u8], bit: usize) -> Vec<u8> {
    let mut flipped = bytes.to_vec();
    let at = (bit / 8) % flipped.len();
    flipped[at] ^= 1 << (bit % 8);
    flipped
}

/// The `proptest` configuration of this crate: at least `cases` cases, and
/// failures saved to a file that is checked in.
pub fn config(cases: u32) -> ProptestConfig {
    let defaults = ProptestConfig::default();
    ProptestConfig {
        cases,
        failure_persistence: Some(Box::new(FileFailurePersistence::Direct(
            "tests/regressions/properties.txt",
        ))),
        ..defaults
    }
}
