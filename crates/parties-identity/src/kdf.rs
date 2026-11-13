//! HKDF-SHA256 as the derivations of this crate use it: no salt, a label as
//! `info`, a short fixed-length output.

use hkdf::Hkdf;
use sha2::Sha256;
use zeroize::Zeroizing;

/// `HKDF-SHA256(salt = none, ikm, info)` with an output of `N` bytes.
///
/// "No salt" is the default of RFC 5869 section 2.2: a salt of 32 zero
/// bytes. The output is wiped when dropped.
pub(crate) fn hkdf_sha256<const N: usize>(ikm: &[u8], info: &[u8]) -> Zeroizing<[u8; N]> {
    // HKDF-SHA256 gives at most 255 blocks of 32 bytes. This is checked
    // where the function is instantiated, so the `expect` cannot be reached.
    const { assert!(N <= 255 * 32, "too long for HKDF-SHA256") };

    let mut output = Zeroizing::new([0u8; N]);
    Hkdf::<Sha256>::new(None, ikm)
        .expand(info, output.as_mut_slice())
        .expect("the output length is checked at compile time");
    output
}

#[cfg(test)]
mod tests {
    use super::hkdf_sha256;

    /// RFC 5869, appendix A.3: SHA-256 with zero-length salt and info.
    #[test]
    fn rfc5869_test_case_3() {
        const OUTPUT: [u8; 42] = [
            0x8d, 0xa4, 0xe7, 0x75, 0xa5, 0x63, 0xc1, 0x8f, 0x71, 0x5f, 0x80, 0x2a, 0x06, 0x3c,
            0x5a, 0x31, 0xb8, 0xa1, 0x1f, 0x5c, 0x5e, 0xe1, 0x87, 0x9e, 0xc3, 0x45, 0x4e, 0x5f,
            0x3c, 0x73, 0x8d, 0x2d, 0x9d, 0x20, 0x13, 0x95, 0xfa, 0xa4, 0xb6, 0x1a, 0x96, 0xc8,
        ];
        let output = hkdf_sha256::<42>(&[0x0b; 22], b"");
        assert_eq!(*output, OUTPUT);
        // A shorter output is a prefix of a longer one.
        let short = hkdf_sha256::<16>(&[0x0b; 22], b"");
        assert_eq!(short.as_slice(), &OUTPUT[..16]);
    }

    #[test]
    fn info_separates_outputs() {
        let first = hkdf_sha256::<32>(b"input key material", b"label one");
        let second = hkdf_sha256::<32>(b"input key material", b"label two");
        assert_ne!(*first, *second);
    }
}
