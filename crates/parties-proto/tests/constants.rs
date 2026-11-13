//! Every number the design document fixes, pinned: stream types, close
//! codes, stream reset codes, limits, rate classes, the ALPN token.
//!
//! These values are the protocol. If one of these tests fails, the change
//! breaks deployed peers, and `docs/rust-rewrite/03-protocol-v2.md` has to
//! change with it.

use std::time::Duration;

use parties_proto::codes::{
    ClientReaction, CloseCode, StreamResetCode, UnknownCloseCode, UnknownStreamResetCode,
};
use parties_proto::limits::{self, RateClass, RateLimit};
use parties_proto::stream::{StreamDirection, StreamType, UnknownStreamType};
use parties_proto::varint::VarInt;
use parties_proto::version;

const KIB: usize = 1024;
const MIB: u64 = 1024 * 1024;

// ---------------------------------------------------------------------------
// Stream types (section 2)
// ---------------------------------------------------------------------------

#[rustfmt::skip]
const STREAM_TYPES: &[(StreamType, u32, StreamDirection, bool)] = &[
    (StreamType::Control,       0x00, StreamDirection::Bidirectional,  true),
    (StreamType::Bulk,          0x01, StreamDirection::Bidirectional,  false),
    (StreamType::Upload,        0x02, StreamDirection::Bidirectional,  false),
    (StreamType::Download,      0x03, StreamDirection::Bidirectional,  false),
    (StreamType::PictureUpload, 0x04, StreamDirection::Bidirectional,  false),
    (StreamType::VideoFrame,    0x10, StreamDirection::Unidirectional, false),
];

#[test]
fn stream_types_are_pinned() {
    assert_eq!(StreamType::ALL.len(), STREAM_TYPES.len());
    for (&(stream_type, code, direction, before_auth), listed) in
        STREAM_TYPES.iter().zip(StreamType::ALL)
    {
        assert_eq!(listed, stream_type);
        assert_eq!(stream_type.code(), code, "{stream_type:?}");
        assert_eq!(stream_type.direction(), direction, "{stream_type:?}");
        assert_eq!(
            stream_type.allowed_before_authentication(),
            before_auth,
            "{stream_type:?}"
        );
        assert_eq!(StreamType::from_code(u64::from(code)), Ok(stream_type));
        assert_eq!(
            StreamType::try_from(VarInt::from_u32(code)),
            Ok(stream_type)
        );
        assert_eq!(VarInt::from(stream_type).get(), u64::from(code));

        // Every type is one byte on the wire: the byte is the number.
        let mut encoded = Vec::new();
        stream_type.encode(&mut encoded);
        assert_eq!(encoded, [u8::try_from(code).expect("one byte")]);
        assert_eq!(StreamType::decode(&encoded), Ok(Some((stream_type, 1))));
    }
}

#[test]
fn an_unknown_stream_type_resets_the_stream_and_keeps_the_connection() {
    let known: Vec<u64> = STREAM_TYPES.iter().map(|row| u64::from(row.1)).collect();
    for code in (0..0x40).filter(|code| !known.contains(code)) {
        let error = StreamType::from_code(code).expect_err("not a stream type");
        assert_eq!(error, UnknownStreamType(code));
        assert_eq!(error.reset_code(), StreamResetCode::UnknownStreamType);
        let byte = u8::try_from(code).expect("one byte");
        assert_eq!(StreamType::decode(&[byte]), Err(UnknownStreamType(code)));
    }
    // A type from a newer version may need more than one byte.
    assert_eq!(
        StreamType::decode(&[0x7B, 0xBD]),
        Err(UnknownStreamType(15_293))
    );
}

#[test]
fn a_stream_type_that_has_not_arrived_yet_is_not_an_error() {
    assert_eq!(StreamType::decode(&[]), Ok(None));
    // The first byte of a two-byte integer.
    assert_eq!(StreamType::decode(&[0x40]), Ok(None));
    // The type may be written on more bytes than it needs.
    assert_eq!(
        StreamType::decode(&[0x40, 0x10]),
        Ok(Some((StreamType::VideoFrame, 2)))
    );
}

// ---------------------------------------------------------------------------
// Close codes and stream reset codes (section 10)
// ---------------------------------------------------------------------------

#[rustfmt::skip]
const CLOSE_CODES: &[(CloseCode, u32, &str, ClientReaction)] = &[
    (CloseCode::Normal,            0, "NORMAL",             ClientReaction::None),
    (CloseCode::ProtocolViolation, 1, "PROTOCOL_VIOLATION", ClientReaction::Report),
    (CloseCode::AuthFailed,        2, "AUTH_FAILED",        ClientReaction::ShowReason),
    (CloseCode::AuthTimeout,       3, "AUTH_TIMEOUT",       ClientReaction::RetryWithBackoff),
    (CloseCode::Kicked,            4, "KICKED",             ClientReaction::StayDisconnected),
    (CloseCode::Banned,            5, "BANNED",             ClientReaction::StayDisconnected),
    (CloseCode::ServerShutdown,    6, "SERVER_SHUTDOWN",    ClientReaction::RetryWithBackoff),
    (CloseCode::RateLimited,       7, "RATE_LIMITED",       ClientReaction::RetryAfterDelay),
    (CloseCode::SessionReplaced,   8, "SESSION_REPLACED",   ClientReaction::StayDisconnected),
    (CloseCode::SlowConsumer,      9, "SLOW_CONSUMER",      ClientReaction::RetryWithBackoff),
];

#[test]
fn close_codes_are_pinned() {
    assert_eq!(CloseCode::ALL.len(), CLOSE_CODES.len());
    for (&(close_code, code, name, reaction), listed) in CLOSE_CODES.iter().zip(CloseCode::ALL) {
        assert_eq!(listed, close_code);
        assert_eq!(close_code.code(), code, "{name}");
        assert_eq!(u32::from(close_code), code, "{name}");
        assert_eq!(close_code.name(), name);
        assert_eq!(close_code.client_reaction(), reaction, "{name}");
        assert_eq!(CloseCode::from_code(u64::from(code)), Ok(close_code));
        assert_eq!(CloseCode::try_from(u64::from(code)), Ok(close_code));
    }
    assert_eq!(CloseCode::from_code(10), Err(UnknownCloseCode(10)));
    assert_eq!(CloseCode::from_code(0x100), Err(UnknownCloseCode(0x100)));
}

#[test]
fn the_codes_a_client_reconnects_after() {
    let reconnecting: Vec<_> = CloseCode::ALL
        .into_iter()
        .filter(|code| code.client_reaction().reconnects())
        .collect();
    assert_eq!(
        reconnecting,
        [
            CloseCode::AuthTimeout,
            CloseCode::ServerShutdown,
            CloseCode::RateLimited,
            CloseCode::SlowConsumer,
        ]
    );
    assert!(!ClientReaction::None.reconnects());
    assert!(!ClientReaction::Report.reconnects());
    assert!(!ClientReaction::ShowReason.reconnects());
    assert!(!ClientReaction::StayDisconnected.reconnects());
}

#[rustfmt::skip]
const STREAM_RESET_CODES: &[(StreamResetCode, u32, &str)] = &[
    (StreamResetCode::Cancelled,         0x100, "CANCELLED"),
    (StreamResetCode::UnknownStreamType, 0x101, "UNKNOWN_STREAM_TYPE"),
    (StreamResetCode::NotAllowed,        0x102, "NOT_ALLOWED"),
    (StreamResetCode::TooLarge,          0x103, "TOO_LARGE"),
    (StreamResetCode::LimitExceeded,     0x104, "LIMIT_EXCEEDED"),
    (StreamResetCode::Malformed,         0x105, "MALFORMED"),
    (StreamResetCode::FrameDropped,      0x106, "FRAME_DROPPED"),
];

#[test]
fn stream_reset_codes_are_pinned() {
    assert_eq!(StreamResetCode::ALL.len(), STREAM_RESET_CODES.len());
    for (&(reset_code, code, name), listed) in STREAM_RESET_CODES.iter().zip(StreamResetCode::ALL) {
        assert_eq!(listed, reset_code);
        assert_eq!(reset_code.code(), code, "{name}");
        assert_eq!(u32::from(reset_code), code, "{name}");
        assert_eq!(reset_code.name(), name);
        assert_eq!(StreamResetCode::from_code(u64::from(code)), Ok(reset_code));
        assert_eq!(StreamResetCode::try_from(u64::from(code)), Ok(reset_code));
    }
    assert_eq!(
        StreamResetCode::from_code(0x107),
        Err(UnknownStreamResetCode(0x107))
    );
}

#[test]
fn close_codes_and_stream_reset_codes_do_not_overlap() {
    for close_code in CloseCode::ALL {
        assert!(close_code.code() < 0x100);
        assert!(StreamResetCode::from_code(u64::from(close_code.code())).is_err());
    }
    for reset_code in StreamResetCode::ALL {
        assert!(reset_code.code() >= 0x100);
        assert!(CloseCode::from_code(u64::from(reset_code.code())).is_err());
    }
}

// ---------------------------------------------------------------------------
// Version (sections 1 and 11)
// ---------------------------------------------------------------------------

#[test]
fn the_transport_profile_is_pinned() {
    assert_eq!(version::ALPN, b"parties/2");
    assert_eq!(version::PROTOCOL_MAJOR, 2);
    assert_eq!(version::PROTOCOL_MINOR, 0);
    assert_eq!(version::DEFAULT_PORT, 7800);
    assert_eq!(limits::IDLE_TIMEOUT, Duration::from_secs(30));
    assert_eq!(limits::KEEP_ALIVE_INTERVAL, Duration::from_secs(10));
    // The ALPN token names the major version.
    assert_eq!(
        version::ALPN,
        format!("parties/{}", version::PROTOCOL_MAJOR).as_bytes()
    );
}

#[test]
fn the_minor_version_is_the_lower_of_the_two() {
    assert_eq!(version::negotiate_minor(0, 0), 0);
    assert_eq!(version::negotiate_minor(3, 1), 1);
    assert_eq!(version::negotiate_minor(1, 3), 1);
}

#[test]
fn features_are_the_ones_both_sides_name() {
    assert!(
        version::FEATURES.is_empty(),
        "2.0 defines no optional feature"
    );
    for feature in version::FEATURES {
        assert!(version::is_valid_feature_name(feature), "{feature}");
    }

    let local = ["resume", "e2ee-media", "simulcast"];
    let remote = [
        "simulcast".to_owned(),
        "unknown".to_owned(),
        "resume".to_owned(),
    ];
    assert_eq!(
        version::negotiate_features(&local, &remote),
        ["resume", "simulcast"]
    );
    assert_eq!(
        version::negotiate_features(&local, &[] as &[&str]),
        [] as [&str; 0]
    );

    assert!(version::is_valid_feature_name("e2ee-media"));
    assert!(version::is_valid_feature_name(&"a".repeat(32)));
    assert!(!version::is_valid_feature_name(&"a".repeat(33)));
    assert!(!version::is_valid_feature_name(""));
    assert!(!version::is_valid_feature_name("Resume"));
    assert!(!version::is_valid_feature_name("two words"));
    assert_eq!(limits::MAX_FEATURES, 64);
    assert_eq!(limits::MAX_FEATURE_NAME_LEN, 32);
}

// ---------------------------------------------------------------------------
// Limits (section 9; 04 sections 6 and 8)
// ---------------------------------------------------------------------------

#[test]
fn limits_before_authentication_are_pinned() {
    assert_eq!(limits::AUTH_DEADLINE, Duration::from_secs(10));
    assert_eq!(limits::MAX_AUTH_ATTEMPTS, 3);
    assert_eq!(limits::MAX_CONTROL_FRAME_LEN_PRE_AUTH, 4 * KIB);
    assert_eq!(limits::MAX_BIDI_STREAMS_PRE_AUTH, 1);
    assert_eq!(limits::MAX_UNI_STREAMS_PRE_AUTH, 0);
}

#[test]
fn limits_after_authentication_are_pinned() {
    assert_eq!(limits::MAX_CONTROL_FRAME_LEN, 64 * KIB);
    assert_eq!(limits::MAX_BULK_FRAME_LEN, 64 * KIB);
    assert_eq!(limits::MAX_BIDI_STREAMS, 16);
    assert_eq!(limits::MAX_UNI_STREAMS, 64);
    assert_eq!(limits::MAX_BULK_STREAMS, 4);
    assert_eq!(limits::MAX_FILE_STREAMS, 4);
    assert_eq!(limits::UNI_STREAM_RECEIVE_WINDOW, 4 * MIB);
    assert_eq!(limits::MAX_READY_FRAMES, 16);
    assert_eq!(limits::MAX_READY_ITEMS, 16_384);
    assert_eq!(limits::MAX_OUTBOX_BYTES, 2048 * KIB);
    // 1 control, 4 bulk and 4 file streams leave spare.
    const {
        assert!(1 + limits::MAX_BULK_STREAMS + limits::MAX_FILE_STREAMS < limits::MAX_BIDI_STREAMS);
    }
    // The largest snapshot fits the outbox with its length prefixes, and
    // leaves nearly as much room again for the events behind it.
    const {
        let snapshot = limits::MAX_READY_FRAMES * (limits::MAX_CONTROL_FRAME_LEN + VarInt::MAX_LEN);
        assert!(snapshot + snapshot / 2 < limits::MAX_OUTBOX_BYTES);
    }
}

#[test]
fn media_limits_are_pinned() {
    assert_eq!(limits::MAX_AUDIO_PAYLOAD_LEN, 1000);
    assert_eq!(limits::MAX_AUDIO_DATAGRAMS_PER_SECOND, 120);
    assert_eq!(limits::AUDIO_CLOCK_RATE, 48_000);
    assert_eq!(limits::MAX_VIDEO_FRAME_STREAM_LEN, 4 * MIB);
    assert_eq!(limits::MAX_BUFFERED_VIDEO_INGRESS, 16 * MIB);
    // A frame stream fits the receive window of its stream.
    const {
        assert!(limits::MAX_VIDEO_FRAME_STREAM_LEN <= limits::UNI_STREAM_RECEIVE_WINDOW);
    }
}

#[test]
fn text_and_profile_limits_are_pinned() {
    assert_eq!(limits::DEFAULT_MAX_MESSAGE_CHARS, 4000);
    assert_eq!(
        (
            limits::MIN_DISPLAY_NAME_CHARS,
            limits::MAX_DISPLAY_NAME_CHARS
        ),
        (1, 32)
    );
    assert_eq!((limits::MIN_NAME_CHARS, limits::MAX_NAME_CHARS), (1, 64));
    assert_eq!(limits::MAX_MEMBERS_IN_READY, 1000);
    assert_eq!(limits::PICTURE_HASH_LEN, 64);
    assert_eq!(limits::MAX_PICTURE_LEN, 128 * KIB);
    assert_eq!(limits::PICTURE_SIDE_PIXELS, 256);
    assert_eq!(limits::JOIN_CODE_LEN, 10);
    assert_eq!(
        limits::DEFAULT_JOIN_CODE_LIFETIME,
        Duration::from_secs(7 * 86_400)
    );
}

#[test]
fn rate_classes_are_pinned() {
    let second = Duration::from_secs(1);
    #[rustfmt::skip]
    let expected: &[(RateClass, u32, Duration, u32)] = &[
        (RateClass::Presence,        5, second,                  10),
        (RateClass::Join,            1, second,                  5),
        (RateClass::ChatSend,        5, Duration::from_secs(5),  5),
        (RateClass::Pages,           2, second,                  4),
        (RateClass::Search,          1, second,                  2),
        (RateClass::Admin,           2, second,                  10),
        (RateClass::KeyframeRequest, 4, second,                  4),
    ];
    assert_eq!(RateClass::ALL.len(), expected.len());
    for (&(class, count, period, burst), listed) in expected.iter().zip(RateClass::ALL) {
        assert_eq!(listed, class);
        assert_eq!(
            class.limit(),
            RateLimit {
                count,
                period,
                burst
            },
            "{class:?}"
        );
    }
    assert_eq!(
        RateClass::Presence.limit().interval(),
        Duration::from_millis(200)
    );
    assert_eq!(RateClass::ChatSend.limit().interval(), second);
    assert_eq!(
        RateLimit::per_second(4, 4).interval(),
        Duration::from_millis(250)
    );
}
