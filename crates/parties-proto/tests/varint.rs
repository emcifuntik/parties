//! QUIC variable-length integers: the vectors of RFC 9000 appendix A.1, the
//! boundaries of the four lengths, and inputs that end too early.

mod common;

use bytes::{Buf, BytesMut};
use common::hex;
use parties_proto::varint::{VarInt, VarIntIncomplete, VarIntRangeError};

fn encode(value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    VarInt::new(value).expect("in range").encode(&mut out);
    out
}

/// RFC 9000 appendix A.1, "Sample Variable-Length Integer Decoding".
const RFC_9000_A1: &[(&str, u64)] = &[
    ("c2197c5eff14e88c", 151_288_809_941_952_652),
    ("9d7f3e7d", 494_878_333),
    ("7bbd", 15_293),
    ("25", 37),
];

#[test]
fn rfc_9000_appendix_a1_vectors_decode() {
    for &(encoded, value) in RFC_9000_A1 {
        let bytes = hex(encoded);
        let (decoded, len) = VarInt::decode(&bytes).expect("a complete integer");
        assert_eq!(decoded.get(), value, "{encoded}");
        assert_eq!(len, bytes.len(), "{encoded}");
    }
}

#[test]
fn rfc_9000_appendix_a1_vectors_encode() {
    // The four vectors are in their shortest form, so encoding gives them
    // back.
    for &(encoded, value) in RFC_9000_A1 {
        assert_eq!(encode(value), hex(encoded), "{value}");
    }
}

#[test]
fn rfc_9000_a_value_may_use_more_bytes_than_it_needs() {
    // "the two-byte sequence 0x4025 also decodes to 37".
    let (decoded, len) = VarInt::decode(&hex("4025")).expect("a complete integer");
    assert_eq!((decoded.get(), len), (37, 2));
    // The same holds for the longer forms.
    let (decoded, len) = VarInt::decode(&hex("80000025")).expect("a complete integer");
    assert_eq!((decoded.get(), len), (37, 4));
    let (decoded, len) = VarInt::decode(&hex("c000000000000025")).expect("a complete integer");
    assert_eq!((decoded.get(), len), (37, 8));
    // Encoding always picks the shortest form.
    assert_eq!(encode(37), hex("25"));
}

/// RFC 9000 section 16, table 4: the largest value of each length.
#[test]
fn length_boundaries() {
    #[rustfmt::skip]
    let cases: &[(u64, &str)] = &[
        (0,                         "00"),
        (63,                        "3f"),
        (64,                        "4040"),
        (16_383,                    "7fff"),
        (16_384,                    "80004000"),
        (1_073_741_823,             "bfffffff"),
        (1_073_741_824,             "c000000040000000"),
        (4_611_686_018_427_387_903, "ffffffffffffffff"),
    ];
    for &(value, encoded) in cases {
        let bytes = hex(encoded);
        assert_eq!(encode(value), bytes, "{value}");
        assert_eq!(
            VarInt::new(value).expect("in range").encoded_len(),
            bytes.len(),
            "{value}"
        );
        assert_eq!(
            VarInt::len_from_first_byte(bytes[0]),
            bytes.len(),
            "{value}"
        );
        assert_eq!(
            VarInt::decode(&bytes).expect("complete").0.get(),
            value,
            "{encoded}"
        );
        let (array, len) = VarInt::new(value).expect("in range").to_bytes();
        assert_eq!(&array[..len], bytes, "{value}");
    }
}

#[test]
fn the_range_ends_at_two_to_the_62_minus_one() {
    assert_eq!(VarInt::MAX.get(), (1 << 62) - 1);
    assert_eq!(VarInt::new(VarInt::MAX.get()), Ok(VarInt::MAX));
    assert_eq!(VarInt::new(1 << 62), Err(VarIntRangeError(1 << 62)));
    assert_eq!(VarInt::try_from(u64::MAX), Err(VarIntRangeError(u64::MAX)));
    assert_eq!(VarInt::try_from(300_usize).map(VarInt::get), Ok(300));
    assert_eq!(VarInt::from(u32::MAX).get(), u64::from(u32::MAX));
    assert_eq!(VarInt::from_u32(7), VarInt::from(7_u8));
    assert_eq!(u64::from(VarInt::from(9_u16)), 9);
}

#[test]
fn an_integer_that_is_cut_off_is_incomplete() {
    assert_eq!(
        VarInt::decode(&[]),
        Err(VarIntIncomplete {
            needed: 1,
            available: 0
        })
    );
    for &(encoded, _) in RFC_9000_A1 {
        let bytes = hex(encoded);
        for available in 1..bytes.len() {
            assert_eq!(
                VarInt::decode(&bytes[..available]),
                Err(VarIntIncomplete {
                    needed: bytes.len(),
                    available
                }),
                "{encoded} cut to {available}"
            );
        }
    }
}

#[test]
fn bytes_after_the_integer_are_left_alone() {
    let (decoded, len) = VarInt::decode(&hex("7bbd ff ff")).expect("complete");
    assert_eq!((decoded.get(), len), (15_293, 2));
}

#[test]
fn decoding_from_a_buffer_advances_it_only_on_success() {
    let mut buffer = BytesMut::from(&hex("9d7f3e7d 25")[..]);
    assert_eq!(
        VarInt::decode_buf(&mut buffer).map(VarInt::get),
        Ok(494_878_333)
    );
    assert_eq!(VarInt::decode_buf(&mut buffer).map(VarInt::get), Ok(37));
    assert_eq!(
        VarInt::decode_buf(&mut buffer),
        Err(VarIntIncomplete {
            needed: 1,
            available: 0
        })
    );

    let mut cut = BytesMut::from(&hex("c2197c5e")[..]);
    assert_eq!(
        VarInt::decode_buf(&mut cut),
        Err(VarIntIncomplete {
            needed: 8,
            available: 4
        })
    );
    assert_eq!(cut.remaining(), 4, "nothing was consumed");
}

#[test]
fn decoding_from_a_buffer_in_pieces() {
    // A buffer whose bytes are not contiguous: the integer spans the seam.
    let bytes = hex("c2197c5eff14e88c");
    let mut chained = (&bytes[..3]).chain(&bytes[3..]);
    assert_eq!(
        VarInt::decode_buf(&mut chained).map(VarInt::get),
        Ok(151_288_809_941_952_652)
    );
    assert!(!chained.has_remaining());
}
