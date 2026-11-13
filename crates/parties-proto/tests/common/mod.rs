//! Helpers shared by the integration tests: a realistic state snapshot and
//! the `proptest` strategies.

// Every test binary uses a different part of this module.
#![allow(dead_code)]

pub mod fixtures;
pub mod strategies;

/// Decodes a hexadecimal string; spaces are ignored.
pub fn hex(text: &str) -> Vec<u8> {
    let digits: Vec<u8> = text
        .chars()
        .filter(|c| !c.is_whitespace())
        .map(|c| u8::try_from(c.to_digit(16).expect("a hexadecimal digit")).expect("one digit"))
        .collect();
    assert!(digits.len().is_multiple_of(2), "an even number of digits");
    digits
        .chunks(2)
        .map(|pair| pair[0] << 4 | pair[1])
        .collect()
}
