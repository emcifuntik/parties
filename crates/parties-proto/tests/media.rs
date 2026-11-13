//! The media plane: golden bytes for every header, round trips, and what
//! happens to reserved bits, unknown types and inputs that end too early.

mod common;

use bytes::{Bytes, BytesMut};
use common::hex;
use parties_proto::codes::StreamResetCode;
use parties_proto::ids::TrackId;
use parties_proto::limits::{MAX_AUDIO_PAYLOAD_LEN, MAX_VIDEO_FRAME_STREAM_LEN};
use parties_proto::media::datagram::{LEVEL_SILENCE, TYPE_AUDIO, TYPE_KEYFRAME_REQUEST};
use parties_proto::media::{
    AudioDatagram, AudioFlags, AudioHeader, Datagram, DatagramError, KeyframeReason,
    KeyframeRequest, VideoCodecId, VideoFlags, VideoFrameHeader, VideoLayer, VideoStreamError,
    VideoStreamHeader,
};
use parties_proto::v2;

// ---------------------------------------------------------------------------
// Audio datagrams
// ---------------------------------------------------------------------------

fn audio_header() -> AudioHeader {
    AudioHeader {
        track: TrackId(5),
        seq: 0x0102,
        timestamp: 0x0A0B_0C0D,
        flags: AudioFlags::END_OF_TALKSPURT,
        level: 0x2A,
    }
}

/// type, track id (one byte), seq, timestamp, flags, level, then the payload.
const AUDIO_GOLDEN: &str = "01 05 0201 0d0c0b0a 01 2a aabbcc";

#[test]
fn audio_datagram_golden_bytes() {
    let audio = AudioDatagram {
        header: audio_header(),
        payload: &[0xAA, 0xBB, 0xCC],
    };
    let mut encoded = Vec::new();
    audio.encode(&mut encoded).expect("the payload is short");
    assert_eq!(encoded, hex(AUDIO_GOLDEN));
    assert_eq!(audio.header.encoded_len(), 10);

    assert_eq!(Datagram::decode(&encoded), Ok(Datagram::Audio(audio)));
}

#[test]
fn audio_datagram_golden_bytes_with_longer_track_ids() {
    #[rustfmt::skip]
    let cases: &[(u32, &str)] = &[
        (63,            "01 3f               0201 0d0c0b0a 01 2a"),
        (64,            "01 4040             0201 0d0c0b0a 01 2a"),
        (16_384,        "01 80004000         0201 0d0c0b0a 01 2a"),
        (u32::MAX,      "01 c0000000ffffffff 0201 0d0c0b0a 01 2a"),
    ];
    for &(track, golden) in cases {
        let header = AudioHeader {
            track: TrackId(track),
            ..audio_header()
        };
        let mut encoded = Vec::new();
        header.encode(&mut encoded);
        assert_eq!(encoded, hex(golden), "track {track}");
        assert_eq!(header.encoded_len(), encoded.len(), "track {track}");
        assert!(encoded.len() <= AudioHeader::MAX_LEN);

        let Ok(Datagram::Audio(decoded)) = Datagram::decode(&encoded) else {
            panic!("track {track} does not decode");
        };
        assert_eq!(decoded.header, header);
        assert!(decoded.payload.is_empty(), "an empty payload is allowed");
    }
    assert_eq!(AudioHeader::MAX_LEN, 17);
}

#[test]
fn the_audio_payload_is_borrowed_not_copied() {
    let mut datagram = BytesMut::new();
    audio_header().encode(&mut datagram);
    datagram.extend_from_slice(&[7; 120]);
    let datagram: Bytes = datagram.freeze();

    let Ok(Datagram::Audio(audio)) = Datagram::decode(&datagram) else {
        panic!("does not decode");
    };
    assert!(datagram.as_ptr_range().contains(&audio.payload.as_ptr()));
    // A receiver that wants to keep the payload takes a reference-counted
    // slice of the datagram.
    let kept = datagram.slice_ref(audio.payload);
    assert_eq!(kept.len(), 120);
    assert_eq!(kept.as_ptr(), audio.payload.as_ptr());
}

#[test]
fn the_audio_payload_is_at_most_1000_bytes() {
    assert_eq!(MAX_AUDIO_PAYLOAD_LEN, 1000);
    let mut datagram = Vec::new();
    audio_header().encode(&mut datagram);
    datagram.extend_from_slice(&[0; 1000]);
    assert!(
        matches!(Datagram::decode(&datagram), Ok(Datagram::Audio(audio)) if audio.payload.len() == 1000)
    );

    datagram.push(0);
    assert_eq!(
        Datagram::decode(&datagram),
        Err(DatagramError::PayloadTooLong { len: 1001 })
    );

    let payload = [0; 1001];
    let too_long = AudioDatagram {
        header: audio_header(),
        payload: &payload,
    };
    let mut out = Vec::new();
    assert_eq!(
        too_long.encode(&mut out),
        Err(DatagramError::PayloadTooLong { len: 1001 })
    );
    assert!(out.is_empty(), "a refused datagram writes nothing");
}

#[test]
fn reserved_audio_flag_bits_are_ignored_and_never_sent() {
    // Flags byte 0xFF: both known bits and six reserved ones.
    let datagram = hex("01 05 0201 0d0c0b0a ff 2a");
    let Ok(Datagram::Audio(audio)) = Datagram::decode(&datagram) else {
        panic!("does not decode");
    };
    assert_eq!(
        audio.header.flags,
        AudioFlags::END_OF_TALKSPURT | AudioFlags::ENCRYPTED
    );
    assert_eq!(audio.header.flags.bits(), 0b11);

    let mut again = Vec::new();
    audio.header.encode(&mut again);
    assert_eq!(again, hex("01 05 0201 0d0c0b0a 03 2a"));
}

#[test]
fn audio_flags() {
    assert_eq!(AudioFlags::END_OF_TALKSPURT.bits(), 0b01);
    assert_eq!(AudioFlags::ENCRYPTED.bits(), 0b10);
    assert_eq!(AudioFlags::KNOWN.bits(), 0b11);
    assert_eq!(AudioFlags::default(), AudioFlags::EMPTY);
    assert!(AudioFlags::EMPTY.is_empty());
    let flags = AudioFlags::EMPTY.with(AudioFlags::ENCRYPTED, true);
    assert!(flags.contains(AudioFlags::ENCRYPTED));
    assert!(!flags.contains(AudioFlags::END_OF_TALKSPURT));
    assert!(flags.with(AudioFlags::ENCRYPTED, false).is_empty());
    assert_eq!(
        format!("{:?}", AudioFlags::KNOWN),
        "AudioFlags(END_OF_TALKSPURT | ENCRYPTED)"
    );
    assert_eq!(format!("{:?}", AudioFlags::EMPTY), "AudioFlags()");
}

#[test]
fn the_audio_level_is_between_0_and_127() {
    assert_eq!(LEVEL_SILENCE, 127);
    let level = |level| AudioHeader {
        level,
        ..audio_header()
    };
    assert_eq!(level(0).level_dbov(), 0);
    assert_eq!(level(127).level_dbov(), 127);
    assert_eq!(
        level(200).level_dbov(),
        127,
        "out of range reads as silence"
    );
}

#[test]
fn a_truncated_audio_datagram_is_an_error() {
    let whole = hex("01 4040 0201 0d0c0b0a 01 2a");
    for len in 1..whole.len() {
        assert_eq!(
            Datagram::decode(&whole[..len]),
            Err(DatagramError::Truncated),
            "cut to {len} bytes"
        );
    }
    assert_eq!(Datagram::decode(&[]), Err(DatagramError::Empty));
}

#[test]
fn a_track_id_wider_than_32_bits_is_an_error() {
    // 2^32 as an eight-byte integer.
    let datagram = hex("01 c000000100000000 0201 0d0c0b0a 01 2a");
    assert_eq!(
        Datagram::decode(&datagram),
        Err(DatagramError::TrackIdOutOfRange(1 << 32))
    );
    let request = hex("02 ffffffffffffffff 01");
    assert_eq!(
        Datagram::decode(&request),
        Err(DatagramError::TrackIdOutOfRange((1 << 62) - 1))
    );
}

// ---------------------------------------------------------------------------
// Keyframe requests
// ---------------------------------------------------------------------------

#[test]
fn keyframe_request_golden_bytes() {
    let request = KeyframeRequest {
        track: TrackId(300),
        reason: KeyframeReason::LOSS,
    };
    let mut encoded = Vec::new();
    request.encode(&mut encoded);
    // type, track id 300 on two bytes, reason.
    assert_eq!(encoded, hex("02 412c 01"));
    assert_eq!(request.encoded_len(), 4);
    assert_eq!(KeyframeRequest::MAX_LEN, 10);
    assert_eq!(
        Datagram::decode(&encoded),
        Ok(Datagram::KeyframeRequest(request))
    );
}

#[test]
fn keyframe_reasons() {
    assert_eq!(KeyframeReason::UNSPECIFIED, KeyframeReason(0));
    assert_eq!(KeyframeReason::LOSS, KeyframeReason(1));
    assert_eq!(KeyframeReason::NEW_VIEWER, KeyframeReason(2));
    assert_eq!(KeyframeReason::BACKLOG_RECOVERY, KeyframeReason(3));
    // A reason this version does not know is still a request.
    assert_eq!(
        Datagram::decode(&hex("02 07 e9")),
        Ok(Datagram::KeyframeRequest(KeyframeRequest {
            track: TrackId(7),
            reason: KeyframeReason(0xE9),
        }))
    );
}

#[test]
fn bytes_after_the_reason_are_ignored() {
    assert_eq!(
        Datagram::decode(&hex("02 07 02 deadbeef")),
        Ok(Datagram::KeyframeRequest(KeyframeRequest {
            track: TrackId(7),
            reason: KeyframeReason::NEW_VIEWER,
        }))
    );
}

#[test]
fn a_truncated_keyframe_request_is_an_error() {
    assert_eq!(Datagram::decode(&hex("02")), Err(DatagramError::Truncated));
    assert_eq!(
        Datagram::decode(&hex("02 41")),
        Err(DatagramError::Truncated)
    );
    assert_eq!(
        Datagram::decode(&hex("02 412c")),
        Err(DatagramError::Truncated)
    );
}

// ---------------------------------------------------------------------------
// Datagram types
// ---------------------------------------------------------------------------

#[test]
fn unknown_datagram_types_are_ignored_not_errors() {
    assert_eq!((TYPE_AUDIO, TYPE_KEYFRAME_REQUEST), (0x01, 0x02));
    for datagram_type in (0..=u8::MAX).filter(|t| ![TYPE_AUDIO, TYPE_KEYFRAME_REQUEST].contains(t))
    {
        // Whatever follows, and even if nothing does.
        assert_eq!(
            Datagram::decode(&[datagram_type]),
            Ok(Datagram::Unknown(datagram_type))
        );
        assert_eq!(
            Datagram::decode(&[datagram_type, 0xFF, 0xFF]),
            Ok(Datagram::Unknown(datagram_type))
        );
    }
}

// ---------------------------------------------------------------------------
// Video frame streams
// ---------------------------------------------------------------------------

fn frame_header() -> VideoFrameHeader {
    VideoFrameHeader {
        frame_seq: 0x0102_0304,
        capture_ts_ms: 0x1122_3344,
        flags: VideoFlags::KEYFRAME,
        layer: VideoLayer::BASE,
        codec: VideoCodecId::AV1,
        width: 1920,
        height: 1080,
    }
}

/// frame_seq, capture_ts_ms, flags, layer, codec, reserved, width, height.
const FRAME_HEADER_GOLDEN: &str = "04030201 44332211 01 00 01 00 8007 3804";

#[test]
fn video_frame_header_golden_bytes() {
    let header = frame_header();
    assert_eq!(VideoFrameHeader::LEN, 16);
    assert_eq!(header.to_bytes().to_vec(), hex(FRAME_HEADER_GOLDEN));
    assert_eq!(VideoFrameHeader::from_bytes(&header.to_bytes()), header);
    assert!(header.is_keyframe());
}

#[test]
fn video_stream_header_golden_bytes() {
    #[rustfmt::skip]
    let cases: &[(u32, &str)] = &[
        (9,        "10 09"),
        (70_000,   "10 80011170"),
        (u32::MAX, "10 c0000000ffffffff"),
    ];
    for &(track, prefix) in cases {
        let header = VideoStreamHeader {
            track: TrackId(track),
            frame: frame_header(),
        };
        let golden = hex(&format!("{prefix} {FRAME_HEADER_GOLDEN}"));
        let mut encoded = Vec::new();
        header.encode(&mut encoded);
        assert_eq!(encoded, golden, "track {track}");
        assert_eq!(header.encoded_len(), golden.len(), "track {track}");
        assert!(encoded.len() <= VideoStreamHeader::MAX_LEN);

        encoded.extend_from_slice(b"payload");
        assert_eq!(
            VideoStreamHeader::decode(&encoded),
            Ok(Some((header, golden.len()))),
            "track {track}"
        );
        assert_eq!(&encoded[golden.len()..], b"payload");
        // A receiver that already consumed the type to route the stream.
        assert_eq!(
            VideoStreamHeader::decode_after_type(&encoded[1..]),
            Ok(Some((header, golden.len() - 1))),
            "track {track}"
        );
    }
    assert_eq!(VideoStreamHeader::MAX_LEN, 25);
}

#[test]
fn every_flag_layer_and_codec_has_its_place() {
    let header = VideoFrameHeader {
        frame_seq: u32::MAX,
        capture_ts_ms: 0,
        flags: VideoFlags::KEYFRAME | VideoFlags::DISCARDABLE | VideoFlags::ENCRYPTED,
        layer: VideoLayer::new(2, 1).expect("both fit a nibble"),
        codec: VideoCodecId::H264,
        width: u16::MAX,
        height: 1,
    };
    assert_eq!(
        header.to_bytes().to_vec(),
        hex("ffffffff 00000000 07 21 03 00 ffff 0100")
    );
    assert_eq!(header.layer.spatial(), 2);
    assert_eq!(header.layer.temporal(), 1);
    assert_eq!(VideoLayer::new(16, 0), None);
    assert_eq!(VideoLayer::new(0, 16), None);
    assert_eq!(VideoLayer::default(), VideoLayer::BASE);

    assert_eq!(VideoFlags::KEYFRAME.bits(), 0b001);
    assert_eq!(VideoFlags::DISCARDABLE.bits(), 0b010);
    assert_eq!(VideoFlags::ENCRYPTED.bits(), 0b100);
    assert!(!frame_header().flags.contains(VideoFlags::DISCARDABLE));
}

#[test]
fn reserved_video_bits_and_the_reserved_byte_are_ignored_and_never_sent() {
    let received: [u8; 16] = hex("04030201 44332211 f9 00 01 ee 8007 3804")
        .try_into()
        .expect("16 bytes");
    let header = VideoFrameHeader::from_bytes(&received);
    assert_eq!(header, frame_header(), "flags 0xF9 read as keyframe only");
    assert_eq!(header.to_bytes().to_vec(), hex(FRAME_HEADER_GOLDEN));
}

#[test]
fn the_codec_numbers_are_those_of_the_control_plane() {
    assert_eq!(VideoCodecId::AV1, VideoCodecId(1));
    assert_eq!(VideoCodecId::H265, VideoCodecId(2));
    assert_eq!(VideoCodecId::H264, VideoCodecId(3));
    for codec in [
        v2::VideoCodec::Av1,
        v2::VideoCodec::H265,
        v2::VideoCodec::H264,
    ] {
        let id = VideoCodecId::from(codec);
        assert_eq!(i32::from(id.0), codec as i32);
        assert_eq!(id.known(), Some(codec));
    }
    assert_eq!(
        VideoCodecId::from(v2::VideoCodec::Unspecified),
        VideoCodecId(0)
    );
    // A codec this version does not know still decodes as a header.
    assert_eq!(VideoCodecId(0).known(), None);
    assert_eq!(VideoCodecId(4).known(), None);
    assert_eq!(VideoCodecId(255).known(), None);
}

#[test]
fn a_video_stream_header_arrives_in_pieces() {
    let header = VideoStreamHeader {
        track: TrackId(70_000),
        frame: frame_header(),
    };
    let mut stream = Vec::new();
    header.encode(&mut stream);
    let header_len = stream.len();
    stream.extend_from_slice(&[0x55; 32]);

    for len in 0..header_len {
        assert_eq!(
            VideoStreamHeader::decode(&stream[..len]),
            Ok(None),
            "{len} bytes"
        );
    }
    for len in header_len..=stream.len() {
        assert_eq!(
            VideoStreamHeader::decode(&stream[..len]),
            Ok(Some((header, header_len))),
            "{len} bytes"
        );
    }
    for len in 0..header_len - 1 {
        assert_eq!(
            VideoStreamHeader::decode_after_type(&stream[1..=len]),
            Ok(None)
        );
    }
}

#[test]
fn a_stream_of_another_type_is_not_a_video_stream() {
    // A type this version knows, but not where a video frame is expected.
    let error = VideoStreamHeader::decode(&hex("00 09")).expect_err("a control stream");
    assert_eq!(error, VideoStreamError::NotAVideoStream(0));
    assert_eq!(error.reset_code(), StreamResetCode::NotAllowed);
    // A type this version does not know.
    let error = VideoStreamHeader::decode(&hex("3f 09")).expect_err("type 0x3f");
    assert_eq!(error, VideoStreamError::NotAVideoStream(0x3F));
    assert_eq!(error.reset_code(), StreamResetCode::UnknownStreamType);
    // The type may be written on more bytes than it needs.
    let mut stream = hex("4010 09");
    stream.extend_from_slice(&frame_header().to_bytes());
    assert_eq!(
        VideoStreamHeader::decode(&stream).map(|decoded| decoded.map(|(header, _)| header.track)),
        Ok(Some(TrackId(9)))
    );
}

#[test]
fn a_video_track_id_wider_than_32_bits_is_an_error() {
    let error = VideoStreamHeader::decode(&hex("10 c000000100000000")).expect_err("2^32");
    assert_eq!(error, VideoStreamError::TrackIdOutOfRange(1 << 32));
    assert_eq!(error.reset_code(), StreamResetCode::Malformed);
}

#[test]
fn the_frame_stream_limit_is_4_mib() {
    assert_eq!(MAX_VIDEO_FRAME_STREAM_LEN, 4 * 1024 * 1024);
}
