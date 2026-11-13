//! The frame codec of control and bulk streams: limits, frames split across
//! reads, and the two distinct errors.

use bytes::{Bytes, BytesMut};
use parties_proto::codes::CloseCode;
use parties_proto::frame::{FrameCodec, FrameError, peek_frame};
use parties_proto::limits::{
    MAX_BULK_FRAME_LEN, MAX_CONTROL_FRAME_LEN, MAX_CONTROL_FRAME_LEN_PRE_AUTH,
};
use parties_proto::prost::Message;
use parties_proto::v2::{
    ClientFrame, ClientHandshake, Hello, JoinVoice, LeaveVoice, UpdateProfile, VideoCodec,
    client_frame::Body,
};

fn request(request_id: u32, body: Body) -> ClientFrame {
    ClientFrame {
        request_id,
        reason: String::new(),
        body: Some(body),
    }
}

fn join_voice(request_id: u32) -> ClientFrame {
    request(
        request_id,
        Body::JoinVoice(JoinVoice {
            channel_id: 0x0123_4567_89AB,
            decode_codecs: vec![VideoCodec::Av1.into(), VideoCodec::H264.into()],
            self_muted: true,
            self_deafened: false,
        }),
    )
}

/// A request whose encoding is `len` bytes long.
fn request_of_len(len: usize) -> ClientFrame {
    let with_name = |name_len: usize| {
        request(
            1,
            Body::UpdateProfile(UpdateProfile {
                display_name: Some("x".repeat(name_len)),
                picture_sha512: None,
            }),
        )
    };
    // The overhead of the wrapping fields depends on the length, so look
    // for the name length that gives exactly `len`.
    let overhead = with_name(len).encoded_len() - len;
    (len.saturating_sub(overhead + 4)..=len)
        .map(with_name)
        .find(|frame| frame.encoded_len() == len)
        .unwrap_or_else(|| panic!("no request encodes to {len} bytes"))
}

#[test]
fn the_named_codecs_carry_the_limits_of_the_document() {
    assert_eq!(FrameCodec::CONTROL_PRE_AUTH.max_len(), 4 * 1024);
    assert_eq!(FrameCodec::CONTROL.max_len(), 64 * 1024);
    assert_eq!(FrameCodec::BULK.max_len(), 64 * 1024);
    assert_eq!(
        FrameCodec::CONTROL_PRE_AUTH.max_len(),
        MAX_CONTROL_FRAME_LEN_PRE_AUTH
    );
    assert_eq!(FrameCodec::CONTROL.max_len(), MAX_CONTROL_FRAME_LEN);
    assert_eq!(FrameCodec::BULK.max_len(), MAX_BULK_FRAME_LEN);
}

#[test]
fn a_frame_is_a_length_and_the_message() {
    let frame = join_voice(7);
    let mut wire = BytesMut::new();
    FrameCodec::CONTROL.encode(&frame, &mut wire).expect("fits");

    let message = frame.encode_to_vec();
    assert!(message.len() < 64, "one length byte");
    assert_eq!(usize::from(wire[0]), message.len());
    assert_eq!(&wire[1..], &message[..]);
}

#[test]
fn frames_round_trip_one_after_the_other() {
    let frames = [
        join_voice(1),
        request(2, Body::LeaveVoice(LeaveVoice {})),
        join_voice(3),
    ];
    let codec = FrameCodec::CONTROL;
    let mut wire = BytesMut::new();
    for frame in &frames {
        codec.encode(frame, &mut wire).expect("fits");
    }
    for frame in &frames {
        assert_eq!(
            codec.decode_message::<ClientFrame>(&mut wire),
            Ok(Some(frame.clone()))
        );
    }
    assert_eq!(codec.decode_message::<ClientFrame>(&mut wire), Ok(None));
    assert!(wire.is_empty());
}

#[test]
fn a_frame_split_across_reads_decodes_once_it_is_complete() {
    let frames = [join_voice(1), request_of_len(300), join_voice(2)];
    let codec = FrameCodec::CONTROL;
    let mut wire = BytesMut::new();
    for frame in &frames {
        codec.encode(frame, &mut wire).expect("fits");
    }
    let wire = wire.freeze();

    // Every way of cutting the bytes into two reads, then byte by byte.
    let mut cuts: Vec<Vec<usize>> = (0..=wire.len()).map(|cut| vec![cut]).collect();
    cuts.push((1..wire.len()).collect());
    for cut in cuts {
        let mut received = BytesMut::new();
        let mut decoded = Vec::new();
        let mut start = 0;
        for end in cut.iter().copied().chain([wire.len()]) {
            received.extend_from_slice(&wire[start..end]);
            start = end;
            while let Some(frame) = codec
                .decode_message::<ClientFrame>(&mut received)
                .expect("the stream is well-formed")
            {
                decoded.push(frame);
            }
        }
        assert_eq!(decoded, frames);
        assert!(received.is_empty());
    }
}

#[test]
fn an_incomplete_frame_leaves_the_buffer_untouched() {
    let codec = FrameCodec::CONTROL;
    let mut wire = BytesMut::new();
    codec.encode(&request_of_len(300), &mut wire).expect("fits");
    let whole = wire.clone();

    // Not even the whole length prefix (it is two bytes for 300).
    let mut partial = BytesMut::from(&whole[..1]);
    assert_eq!(codec.decode(&mut partial), Ok(None));
    assert_eq!(&partial[..], &whole[..1]);

    // The prefix and a part of the message.
    let mut partial = BytesMut::from(&whole[..200]);
    assert_eq!(codec.decode(&mut partial), Ok(None));
    assert_eq!(&partial[..], &whole[..200]);

    assert_eq!(codec.decode(&mut BytesMut::new()), Ok(None));
}

#[test]
fn a_frame_of_exactly_the_limit_passes_and_one_byte_more_does_not() {
    let codec = FrameCodec::CONTROL_PRE_AUTH;
    let limit = codec.max_len();

    let at_limit = request_of_len(limit);
    let mut wire = BytesMut::new();
    codec
        .encode(&at_limit, &mut wire)
        .expect("exactly the limit fits");
    assert_eq!(
        codec.decode_message::<ClientFrame>(&mut wire),
        Ok(Some(at_limit))
    );

    let over = request_of_len(limit + 1);
    let mut wire = BytesMut::new();
    assert_eq!(
        codec.encode(&over, &mut wire),
        Err(FrameError::TooLong {
            len: 4097,
            max: 4096
        })
    );
    assert!(wire.is_empty(), "a refused frame writes nothing");

    // The same frame is fine once authentication raised the limit.
    let mut codec = codec;
    codec.set_max_len(MAX_CONTROL_FRAME_LEN);
    codec
        .encode(&over, &mut wire)
        .expect("fits after authentication");
    assert_eq!(
        codec.decode_message::<ClientFrame>(&mut wire),
        Ok(Some(over))
    );
}

#[test]
fn a_frame_that_is_too_long_is_refused_on_its_length_alone() {
    // A sender with a higher limit, a receiver before authentication.
    let mut wire = BytesMut::new();
    FrameCodec::CONTROL
        .encode(&request_of_len(5000), &mut wire)
        .expect("fits the sender");

    // The receiver has seen nothing but the two bytes of the length.
    let mut received = BytesMut::from(&wire[..2]);
    let error = FrameCodec::CONTROL_PRE_AUTH
        .decode(&mut received)
        .expect_err("the length is over the limit");
    assert_eq!(
        error,
        FrameError::TooLong {
            len: 5000,
            max: 4096
        }
    );
    assert_eq!(error.close_code(), CloseCode::ProtocolViolation);
}

#[test]
fn the_largest_length_a_prefix_can_announce_is_refused() {
    // 2^62 - 1 bytes: the receiver must not try to buffer that.
    let mut received = BytesMut::from(&[0xFF; 8][..]);
    assert_eq!(
        FrameCodec::CONTROL.decode(&mut received),
        Err(FrameError::TooLong {
            len: (1 << 62) - 1,
            max: 65_536
        })
    );
    assert_eq!(
        peek_frame(&[0xFF; 8], usize::MAX),
        Ok(None),
        "with no limit it is merely incomplete"
    );
}

#[test]
fn a_frame_that_does_not_decode_is_a_different_error() {
    // Length 2, then a field key that announces a length-delimited field of
    // 5 bytes where none follow.
    let mut received = BytesMut::from(&[0x02, 0x12, 0x05][..]);
    let error = FrameCodec::CONTROL
        .decode_message::<ClientFrame>(&mut received)
        .expect_err("the message is cut off inside the frame");
    assert!(matches!(error, FrameError::Malformed(_)), "{error:?}");
    assert_eq!(error.close_code(), CloseCode::ProtocolViolation);
    assert!(error.to_string().starts_with("a frame does not decode"));

    // Invalid UTF-8 in a string field (`reason`, field 2).
    let mut received = BytesMut::from(&[0x04, 0x12, 0x02, 0xFF, 0xFE][..]);
    let error = FrameCodec::CONTROL
        .decode_message::<ClientFrame>(&mut received)
        .expect_err("strings are UTF-8");
    assert!(matches!(error, FrameError::Malformed(_)), "{error:?}");
}

#[test]
fn an_empty_frame_is_a_message_of_defaults() {
    let codec = FrameCodec::CONTROL;
    let mut wire = BytesMut::new();
    codec
        .encode(&ClientFrame::default(), &mut wire)
        .expect("fits");
    assert_eq!(&wire[..], [0x00]);
    assert_eq!(codec.decode(&mut wire.clone()), Ok(Some(Bytes::new())));
    assert_eq!(
        codec.decode_message::<ClientFrame>(&mut wire),
        Ok(Some(ClientFrame::default()))
    );
}

#[test]
fn unknown_fields_are_ignored() {
    // A `ClientFrame` from a newer client: request_id 5, then a body this
    // version does not know (field 900, length-delimited, 3 bytes).
    let message = [0x08, 0x05, 0xA2, 0x38, 0x03, 0x01, 0x02, 0x03];
    let mut wire = BytesMut::new();
    FrameCodec::CONTROL
        .encode_bytes(&message, &mut wire)
        .expect("fits");
    let decoded = FrameCodec::CONTROL
        .decode_message::<ClientFrame>(&mut wire)
        .expect("unknown fields are not an error")
        .expect("complete");
    assert_eq!(
        decoded,
        ClientFrame {
            request_id: 5,
            reason: String::new(),
            body: None
        }
    );
}

#[test]
fn encoded_bytes_and_messages_frame_alike() {
    let frame = join_voice(9);
    let (mut from_message, mut from_bytes) = (BytesMut::new(), BytesMut::new());
    FrameCodec::BULK
        .encode(&frame, &mut from_message)
        .expect("fits");
    FrameCodec::BULK
        .encode_bytes(&frame.encode_to_vec(), &mut from_bytes)
        .expect("fits");
    assert_eq!(from_message, from_bytes);

    let too_long = vec![0; MAX_BULK_FRAME_LEN + 1];
    assert_eq!(
        FrameCodec::BULK.encode_bytes(&too_long, &mut from_bytes),
        Err(FrameError::TooLong {
            len: 65_537,
            max: 65_536
        })
    );
}

#[test]
fn bytes_fields_are_slices_of_the_frame() {
    // A `Hello` is a few strings; a handshake frame with a 64-byte hash
    // shows that `Bytes` fields do not copy.
    use parties_proto::v2::{Authenticate, client_handshake};
    let hash = Bytes::from(vec![0xAB; 64]);
    let handshake = ClientHandshake::from(Authenticate {
        picture_sha512: hash.clone(),
        ..Authenticate::default()
    });
    let mut wire = BytesMut::new();
    FrameCodec::CONTROL_PRE_AUTH
        .encode(&handshake, &mut wire)
        .expect("fits");
    let range = wire.as_ptr_range();

    let decoded: ClientHandshake = FrameCodec::CONTROL_PRE_AUTH
        .decode_message(&mut wire)
        .expect("decodes")
        .expect("complete");
    let Some(client_handshake::Kind::Authenticate(authenticate)) = decoded.kind else {
        panic!("not an Authenticate");
    };
    assert_eq!(authenticate.picture_sha512, hash);
    assert!(
        range.contains(&authenticate.picture_sha512.as_ptr()),
        "the hash points into the received buffer"
    );

    let hello = ClientHandshake::from(Hello::default());
    assert!(matches!(hello.kind, Some(client_handshake::Kind::Hello(_))));
}
