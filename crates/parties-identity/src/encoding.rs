//! Small text and byte helpers shared by the modules: lowercase hex, the
//! length error, and "label, zero byte, fields" message building.

use core::fmt;

/// A byte string or a text had the wrong length.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
#[error("expected a length of {expected}, found {found}")]
pub struct LengthError {
    /// The only length that is accepted, in bytes (or characters, for text).
    pub expected: usize,
    /// The length that was given.
    pub found: usize,
}

impl LengthError {
    /// Returns the slice as an array of exactly `N` bytes.
    pub(crate) fn check<const N: usize>(bytes: &[u8]) -> Result<&[u8; N], Self> {
        bytes.try_into().map_err(|_| Self {
            expected: N,
            found: bytes.len(),
        })
    }
}

/// Why a hex string did not decode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HexError {
    /// Not exactly two characters per byte of output.
    Length(LengthError),
    /// A character that is not `0-9`, `a-f` or `A-F`.
    Digit,
}

const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

/// The two lowercase hex digits of a byte.
pub(crate) const fn hex_pair(byte: u8) -> [char; 2] {
    [
        HEX_DIGITS[(byte >> 4) as usize] as char,
        HEX_DIGITS[(byte & 0x0F) as usize] as char,
    ]
}

/// The value of one hex digit, either case.
pub(crate) const fn hex_value(digit: u8) -> Option<u8> {
    match digit {
        b'0'..=b'9' => Some(digit - b'0'),
        b'a'..=b'f' => Some(digit - b'a' + 10),
        b'A'..=b'F' => Some(digit - b'A' + 10),
        _ => None,
    }
}

/// Writes bytes as lowercase hex without separators.
pub(crate) fn write_hex(f: &mut fmt::Formatter<'_>, bytes: &[u8]) -> fmt::Result {
    use fmt::Write as _;

    for &byte in bytes {
        let [high, low] = hex_pair(byte);
        f.write_char(high)?;
        f.write_char(low)?;
    }
    Ok(())
}

/// Decodes exactly `out.len()` bytes of hex, either case, into `out`.
///
/// On an error `out` may hold a decoded prefix; callers that decode secrets
/// pass a buffer that is wiped when dropped.
pub(crate) fn decode_hex(text: &str, out: &mut [u8]) -> Result<(), HexError> {
    let digits = text.as_bytes();
    if digits.len() != out.len() * 2 {
        return Err(HexError::Length(LengthError {
            expected: out.len() * 2,
            found: digits.len(),
        }));
    }
    for (byte, pair) in out.iter_mut().zip(digits.chunks_exact(2)) {
        let high = hex_value(pair[0]).ok_or(HexError::Digit)?;
        let low = hex_value(pair[1]).ok_or(HexError::Digit)?;
        *byte = (high << 4) | low;
    }
    Ok(())
}

/// Builds a signed or hashed message of a fixed length: the parts one after
/// the other.
///
/// Every message of this crate is `label || 0x00 || fixed-length fields`. The
/// labels contain no zero byte, so no message of one kind can be read as a
/// message of another kind.
///
/// The total length is part of the type. A mismatch between `N` and the
/// parts is a programming error in this crate, caught by the tests of every
/// message; the function then fills what fits and leaves the rest zero
/// instead of panicking.
pub(crate) fn concat<const N: usize>(parts: &[&[u8]]) -> [u8; N] {
    let mut message = [0u8; N];
    let mut rest = message.as_mut_slice();
    for part in parts {
        let take = part.len().min(rest.len());
        let (head, tail) = rest.split_at_mut(take);
        head.copy_from_slice(&part[..take]);
        rest = tail;
    }
    debug_assert!(
        rest.is_empty() && parts.iter().map(|part| part.len()).sum::<usize>() == N,
        "message parts do not add up to the declared length"
    );
    message
}

#[cfg(test)]
mod tests {
    use super::{HexError, LengthError, concat, decode_hex, hex_pair, hex_value};

    #[test]
    fn hex_round_trips_every_byte() {
        for byte in 0..=u8::MAX {
            let [high, low] = hex_pair(byte);
            let text: String = [high, low].iter().collect();
            let mut out = [0u8; 1];
            assert_eq!(decode_hex(&text, &mut out), Ok(()));
            assert_eq!(out[0], byte);
            assert_eq!(decode_hex(&text.to_ascii_uppercase(), &mut out), Ok(()));
            assert_eq!(out[0], byte);
        }
    }

    #[test]
    fn hex_rejects_wrong_length_and_wrong_digits() {
        let mut out = [0u8; 2];
        assert_eq!(
            decode_hex("abc", &mut out),
            Err(HexError::Length(LengthError {
                expected: 4,
                found: 3
            }))
        );
        assert_eq!(decode_hex("abcg", &mut out), Err(HexError::Digit));
        assert_eq!(decode_hex("ab c", &mut out), Err(HexError::Digit));
        assert_eq!(hex_value(b'g'), None);
        assert_eq!(hex_value(b'F'), Some(15));
    }

    #[test]
    fn concat_joins_parts_in_order() {
        let message: [u8; 6] = concat(&[b"ab", &[0], b"cde"]);
        assert_eq!(&message, b"ab\0cde");
    }

    #[test]
    fn length_check_reports_both_lengths() {
        assert_eq!(
            LengthError::check::<4>(&[1, 2, 3]),
            Err(LengthError {
                expected: 4,
                found: 3
            })
        );
        assert_eq!(LengthError::check::<3>(&[1, 2, 3]), Ok(&[1, 2, 3]));
    }
}
