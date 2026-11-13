//! Property tests for everything that reads bytes from the network:
//!
//! - a decoder never panics, whatever the input, and never reads past it;
//! - encoding and then decoding gives back what was encoded;
//! - what a decoder accepted, encoded again, decodes to the same value.

mod common;

use bytes::{Buf, BytesMut};
use common::strategies::{
    audio_header, client_frame, config, keyframe_request, ready, server_frame, track_id,
    varint_value, video_frame_header, video_stream_header,
};
use parties_proto::frame::{FrameCodec, FrameError, peek_frame};
use parties_proto::ids::TimeSortableId;
use parties_proto::limits::{MAX_AUDIO_PAYLOAD_LEN, MAX_READY_FRAMES};
use parties_proto::media::{
    AudioDatagram, Datagram, DatagramError, VideoFrameHeader, VideoStreamHeader,
};
use parties_proto::prost::Message;
use parties_proto::snapshot::ReadyAssembler;
use parties_proto::stream::StreamType;
use parties_proto::v2::{ClientFrame, ClientHandshake, Ready, ServerFrame, ServerHandshake};
use parties_proto::varint::VarInt;
use proptest::collection::vec;
use proptest::prelude::*;
use proptest::sample::Index;

const REGRESSIONS: &str = "tests/regressions/properties.txt";

/// Decodes `bytes` as `M`. When that works, the message encodes to
/// something that decodes to the same message again.
fn decode_is_stable<M: Message + Default + PartialEq>(bytes: &[u8]) -> Result<(), TestCaseError> {
    if let Ok(message) = M::decode(bytes) {
        let again = M::decode(message.encode_to_vec().as_slice());
        prop_assert!(
            again.as_ref() == Ok(&message),
            "re-encoding changed the message"
        );
    }
    Ok(())
}

/// Cuts `bytes` into the reads a receiver might see: `cuts` picks where.
fn reads<'a>(bytes: &'a [u8], cuts: &[Index]) -> Vec<&'a [u8]> {
    let mut positions: Vec<usize> = cuts.iter().map(|cut| cut.index(bytes.len() + 1)).collect();
    positions.push(bytes.len());
    positions.sort_unstable();
    let mut start = 0;
    positions
        .into_iter()
        .map(|end| {
            let read = &bytes[start..end];
            start = end;
            read
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Variable-length integers and stream types
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(1024, REGRESSIONS))]

    #[test]
    fn a_varint_round_trips(value in varint_value(), trailing in vec(any::<u8>(), 0..4)) {
        let varint = VarInt::new(value).expect("in range");
        let mut encoded = Vec::new();
        varint.encode(&mut encoded);
        prop_assert_eq!(encoded.len(), varint.encoded_len());
        prop_assert_eq!(VarInt::len_from_first_byte(encoded[0]), encoded.len());
        let (array, len) = varint.to_bytes();
        prop_assert_eq!(&array[..len], &encoded[..]);

        // What follows the integer does not change it.
        let len = encoded.len();
        encoded.extend_from_slice(&trailing);
        prop_assert_eq!(VarInt::decode(&encoded), Ok((varint, len)));
        let mut buffer = &encoded[..];
        prop_assert_eq!(VarInt::decode_buf(&mut buffer), Ok(varint));
        prop_assert_eq!(buffer, &trailing[..]);

        // Every shorter input is incomplete, not a different value.
        for cut in 0..len {
            prop_assert!(VarInt::decode(&encoded[..cut]).is_err());
        }
    }

    #[test]
    fn a_varint_is_never_out_of_range(value in (1_u64 << 62)..) {
        prop_assert!(VarInt::new(value).is_err());
    }

    #[test]
    fn decoding_a_varint_never_panics(bytes in vec(any::<u8>(), 0..12)) {
        let from_slice = VarInt::decode(&bytes);
        if let Ok((value, len)) = from_slice {
            prop_assert!(len <= bytes.len());
            prop_assert!([1, 2, 4, 8].contains(&len));
            prop_assert!(value <= VarInt::MAX);
            // The shortest form of the value decodes to the same value.
            let mut shortest = Vec::new();
            value.encode(&mut shortest);
            prop_assert!(shortest.len() <= len);
            prop_assert_eq!(VarInt::decode(&shortest), Ok((value, shortest.len())));
        }
        // The two decoders agree, and the buffer moves only on success.
        let mut buffer = &bytes[..];
        let from_buffer = VarInt::decode_buf(&mut buffer);
        prop_assert_eq!(from_buffer, from_slice.map(|(value, _)| value));
        let consumed = bytes.len() - buffer.remaining();
        prop_assert_eq!(consumed, from_slice.map_or(0, |(_, len)| len));
    }

    #[test]
    fn decoding_a_stream_type_never_panics(bytes in vec(any::<u8>(), 0..12)) {
        match StreamType::decode(&bytes) {
            Ok(Some((stream_type, len))) => {
                prop_assert!(len <= bytes.len());
                let (code, _) = VarInt::decode(&bytes).expect("it decoded");
                prop_assert_eq!(u64::from(stream_type.code()), code.get());
            }
            Ok(None) => prop_assert!(VarInt::decode(&bytes).is_err()),
            Err(unknown) => prop_assert!(StreamType::from_code(unknown.0).is_err()),
        }
    }
}

// ---------------------------------------------------------------------------
// Media plane
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(1024, REGRESSIONS))]

    #[test]
    fn an_audio_datagram_round_trips(
        header in audio_header(),
        payload in vec(any::<u8>(), 0..=MAX_AUDIO_PAYLOAD_LEN),
    ) {
        let audio = AudioDatagram { header, payload: &payload };
        let mut encoded = Vec::new();
        prop_assert_eq!(audio.encode(&mut encoded), Ok(()));
        prop_assert_eq!(encoded.len(), header.encoded_len() + payload.len());
        prop_assert_eq!(Datagram::decode(&encoded), Ok(Datagram::Audio(audio)));

        // The header alone, with the payload written behind it, is the same
        // datagram.
        let mut by_parts = Vec::new();
        header.encode(&mut by_parts);
        by_parts.extend_from_slice(&payload);
        prop_assert_eq!(by_parts, encoded);
    }

    #[test]
    fn an_audio_payload_over_the_limit_is_refused(
        header in audio_header(),
        extra in 1_usize..64,
    ) {
        let payload = vec![0; MAX_AUDIO_PAYLOAD_LEN + extra];
        let len = payload.len();
        let audio = AudioDatagram { header, payload: &payload };
        let mut encoded = Vec::new();
        prop_assert_eq!(audio.encode(&mut encoded), Err(DatagramError::PayloadTooLong { len }));
        prop_assert!(encoded.is_empty());

        header.encode(&mut encoded);
        encoded.extend_from_slice(&payload);
        prop_assert_eq!(Datagram::decode(&encoded), Err(DatagramError::PayloadTooLong { len }));
    }

    #[test]
    fn a_keyframe_request_round_trips(
        request in keyframe_request(),
        trailing in vec(any::<u8>(), 0..8),
    ) {
        let mut encoded = Vec::new();
        request.encode(&mut encoded);
        prop_assert_eq!(encoded.len(), request.encoded_len());
        // Bytes a later version appends are ignored.
        encoded.extend_from_slice(&trailing);
        prop_assert_eq!(Datagram::decode(&encoded), Ok(Datagram::KeyframeRequest(request)));
    }

    #[test]
    fn decoding_a_datagram_never_panics(bytes in vec(any::<u8>(), 0..1100)) {
        match Datagram::decode(&bytes) {
            Ok(Datagram::Audio(audio)) => {
                prop_assert!(audio.payload.len() <= MAX_AUDIO_PAYLOAD_LEN);
                // The payload is the tail of the input, not a copy.
                let tail = &bytes[bytes.len() - audio.payload.len()..];
                prop_assert_eq!(audio.payload.as_ptr(), tail.as_ptr());
                // What was accepted encodes to something that decodes to
                // the same datagram.
                let mut again = Vec::new();
                prop_assert_eq!(audio.encode(&mut again), Ok(()));
                prop_assert_eq!(Datagram::decode(&again), Ok(Datagram::Audio(audio)));
            }
            Ok(Datagram::KeyframeRequest(request)) => {
                let mut again = Vec::new();
                request.encode(&mut again);
                prop_assert_eq!(Datagram::decode(&again), Ok(Datagram::KeyframeRequest(request)));
            }
            Ok(Datagram::Unknown(datagram_type)) => {
                prop_assert_eq!(datagram_type, bytes[0]);
                prop_assert!(!(1..=2).contains(&datagram_type));
            }
            Err(DatagramError::Empty) => prop_assert!(bytes.is_empty()),
            Err(_) => prop_assert!((1..=2).contains(&bytes[0])),
        }
    }

    #[test]
    fn decoding_a_datagram_of_a_known_type_never_panics(
        datagram_type in 1_u8..=2,
        body in vec(any::<u8>(), 0..40),
    ) {
        // Random bytes rarely start with a known type; these always do.
        let mut bytes = vec![datagram_type];
        bytes.extend_from_slice(&body);
        let _ = Datagram::decode(&bytes);
    }

    #[test]
    fn any_16_bytes_are_a_video_frame_header(bytes in any::<[u8; 16]>()) {
        let header = VideoFrameHeader::from_bytes(&bytes);
        // Encoding clears what is reserved and keeps everything else.
        let encoded = header.to_bytes();
        prop_assert_eq!(VideoFrameHeader::from_bytes(&encoded), header);
        prop_assert_eq!(encoded[11], 0, "the reserved byte");
        prop_assert_eq!(encoded[8], bytes[8] & 0b111, "the flags");
        for index in (0..16).filter(|index| ![8, 11].contains(index)) {
            prop_assert_eq!(encoded[index], bytes[index], "byte {}", index);
        }
    }

    #[test]
    fn a_video_frame_header_round_trips(header in video_frame_header()) {
        prop_assert_eq!(VideoFrameHeader::from_bytes(&header.to_bytes()), header);
    }

    #[test]
    fn a_video_stream_header_round_trips(
        header in video_stream_header(),
        payload in vec(any::<u8>(), 0..64),
    ) {
        let mut stream = Vec::new();
        header.encode(&mut stream);
        let header_len = stream.len();
        prop_assert_eq!(header_len, header.encoded_len());
        prop_assert!(header_len <= VideoStreamHeader::MAX_LEN);
        stream.extend_from_slice(&payload);

        prop_assert_eq!(VideoStreamHeader::decode(&stream), Ok(Some((header, header_len))));
        prop_assert_eq!(&stream[header_len..], &payload[..]);
        prop_assert_eq!(
            VideoStreamHeader::decode_after_type(&stream[1..]),
            Ok(Some((header, header_len - 1)))
        );
        // Until the header is complete, the decoder asks for more.
        for cut in 0..header_len {
            prop_assert_eq!(VideoStreamHeader::decode(&stream[..cut]), Ok(None));
        }
    }

    #[test]
    fn decoding_a_video_stream_header_never_panics(
        bytes in vec(any::<u8>(), 0..48),
        track in track_id(),
    ) {
        if let Ok(Some((header, len))) = VideoStreamHeader::decode(&bytes) {
            prop_assert!(len <= bytes.len());
            let mut again = Vec::new();
            header.encode(&mut again);
            prop_assert_eq!(VideoStreamHeader::decode(&again), Ok(Some((header, again.len()))));
        }
        if let Ok(Some((_, len))) = VideoStreamHeader::decode_after_type(&bytes) {
            prop_assert!(len <= bytes.len());
        }

        // Random bytes rarely start with the right type and a track id that
        // fits; these always do.
        let mut stream = Vec::new();
        StreamType::VideoFrame.encode(&mut stream);
        VarInt::from_u32(track.0).encode(&mut stream);
        stream.extend_from_slice(&bytes);
        match VideoStreamHeader::decode(&stream) {
            Ok(Some((header, _))) => {
                prop_assert_eq!(header.track, track);
                prop_assert!(bytes.len() >= VideoFrameHeader::LEN);
            }
            Ok(None) => prop_assert!(bytes.len() < VideoFrameHeader::LEN),
            Err(error) => prop_assert!(false, "{error}"),
        }
    }
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(512, REGRESSIONS))]

    #[test]
    fn frames_survive_any_split_across_reads(
        messages in vec(vec(any::<u8>(), 0..300), 0..8),
        cuts in vec(any::<Index>(), 0..6),
    ) {
        let codec = FrameCodec::new(299);
        let mut wire = BytesMut::new();
        for message in &messages {
            codec.encode_bytes(message, &mut wire).expect("at most 299 bytes");
        }

        let mut received = BytesMut::new();
        let mut decoded = Vec::new();
        for read in reads(&wire, &cuts) {
            received.extend_from_slice(read);
            while let Some(frame) = codec.decode(&mut received).expect("well-formed") {
                decoded.push(frame.to_vec());
            }
        }
        prop_assert_eq!(decoded, messages);
        prop_assert!(received.is_empty());
    }

    #[test]
    fn the_frame_decoder_never_panics(
        bytes in vec(any::<u8>(), 0..600),
        max_len in 0_usize..200,
        cuts in vec(any::<Index>(), 0..4),
    ) {
        let codec = FrameCodec::new(max_len);
        let mut received = BytesMut::new();
        'stream: for read in reads(&bytes, &cuts) {
            received.extend_from_slice(read);
            loop {
                let before = received.len();
                match codec.decode(&mut received) {
                    Ok(Some(frame)) => {
                        prop_assert!(frame.len() <= max_len);
                        prop_assert!(received.len() < before, "a frame consumes its bytes");
                    }
                    Ok(None) => {
                        prop_assert_eq!(received.len(), before, "nothing is consumed");
                        // What is kept waiting is bounded: the length prefix
                        // and at most one message.
                        prop_assert!(received.len() < VarInt::MAX_LEN + max_len);
                        break;
                    }
                    Err(FrameError::TooLong { len, max }) => {
                        prop_assert!(len > max_len as u64);
                        prop_assert_eq!(max, max_len);
                        // The connection is closed: the stream is not read
                        // any further.
                        break 'stream;
                    }
                    Err(FrameError::Malformed(error)) => {
                        prop_assert!(false, "splitting does not decode: {error}");
                    }
                }
            }
        }
    }

    #[test]
    fn peeking_agrees_with_decoding(bytes in vec(any::<u8>(), 0..300), max_len in 0_usize..200) {
        let peeked = peek_frame(&bytes, max_len);
        let mut buffer = BytesMut::from(&bytes[..]);
        let decoded = FrameCodec::new(max_len).decode(&mut buffer);
        match (peeked, decoded) {
            (Ok(Some((prefix_len, len))), Ok(Some(frame))) => {
                prop_assert_eq!(&bytes[prefix_len..prefix_len + len], &frame[..]);
                prop_assert_eq!(buffer.len(), bytes.len() - prefix_len - len);
            }
            (Ok(None), Ok(None)) => prop_assert_eq!(&buffer[..], &bytes[..]),
            (Err(peeked), Err(decoded)) => prop_assert_eq!(peeked, decoded),
            (peeked, decoded) => prop_assert!(false, "{peeked:?} against {decoded:?}"),
        }
    }
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(256, REGRESSIONS))]

    #[test]
    fn a_request_round_trips_through_its_frame(frame in client_frame()) {
        let codec = FrameCodec::CONTROL;
        let mut wire = BytesMut::new();
        codec.encode(&frame, &mut wire).expect("fits");
        prop_assert_eq!(codec.decode_message::<ClientFrame>(&mut wire), Ok(Some(frame)));
        prop_assert!(wire.is_empty());
    }

    #[test]
    fn a_response_or_an_event_round_trips_through_its_frame(frame in server_frame()) {
        let codec = FrameCodec::CONTROL;
        let mut wire = BytesMut::new();
        codec.encode(&frame, &mut wire).expect("fits");
        prop_assert_eq!(codec.decode_message::<ServerFrame>(&mut wire), Ok(Some(frame)));
        prop_assert!(wire.is_empty());
    }

    #[test]
    fn a_snapshot_round_trips_and_reassembles(
        ready in ready(),
        cuts in vec(any::<Index>(), 0..MAX_READY_FRAMES - 1),
    ) {
        let decoded = Ready::decode(ready.encode_to_vec().as_slice());
        prop_assert_eq!(decoded.as_ref(), Ok(&ready));

        // Cut the member directory at arbitrary places into frames, as a
        // server does with a large snapshot.
        let mut bounds: Vec<usize> =
            cuts.iter().map(|cut| cut.index(ready.members.len() + 1)).collect();
        bounds.sort_unstable();
        let mut parts = vec![Ready { members: Vec::new(), ..ready.clone() }];
        let mut start = 0;
        for end in bounds.into_iter().chain([ready.members.len()]) {
            parts.push(Ready { members: ready.members[start..end].to_vec(), ..Ready::default() });
            start = end;
        }
        let last = parts.len() - 1;
        let mut assembler = ReadyAssembler::new();
        let mut assembled = None;
        for (index, mut part) in parts.into_iter().enumerate() {
            part.more = index != last;
            prop_assert!(assembled.is_none());
            // Each frame crosses the wire on its own.
            let part = Ready::decode(part.encode_to_vec().as_slice()).expect("decodes");
            assembled = assembler.push(part).expect("at most 16 frames");
        }
        prop_assert_eq!(assembled, Some(ready));
    }

    #[test]
    fn decoding_a_message_from_arbitrary_bytes_never_panics(bytes in vec(any::<u8>(), 0..400)) {
        decode_is_stable::<ClientHandshake>(&bytes)?;
        decode_is_stable::<ServerHandshake>(&bytes)?;
        decode_is_stable::<ClientFrame>(&bytes)?;
        decode_is_stable::<ServerFrame>(&bytes)?;
    }

    #[test]
    fn decoding_a_damaged_message_never_panics(
        frame in client_frame(),
        snapshot in ready(),
        damage in vec((any::<Index>(), any::<u8>()), 1..6),
        cut in any::<Index>(),
    ) {
        // Arbitrary bytes are rarely deep protobuf. A valid encoding with a
        // few bytes changed and its end cut off is.
        for mut bytes in [frame.encode_to_vec(), ServerHandshake::from(snapshot).encode_to_vec()] {
            if bytes.is_empty() {
                continue;
            }
            for (position, value) in &damage {
                let position = position.index(bytes.len());
                bytes[position] = *value;
            }
            bytes.truncate(cut.index(bytes.len() + 1));
            decode_is_stable::<ClientHandshake>(&bytes)?;
            decode_is_stable::<ServerHandshake>(&bytes)?;
            decode_is_stable::<ClientFrame>(&bytes)?;
            decode_is_stable::<ServerFrame>(&bytes)?;
        }
    }
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

fn id_parts() -> impl Strategy<Value = (u64, u32)> {
    (
        0..=TimeSortableId::MAX_TIMESTAMP_MS,
        0..=TimeSortableId::MAX_SEQUENCE,
    )
        .prop_filter("0 is not an id", |parts| *parts != (0, 0))
}

proptest! {
    #![proptest_config(config(1024, REGRESSIONS))]

    #[test]
    fn an_id_keeps_its_parts((timestamp_ms, sequence) in id_parts()) {
        let id = TimeSortableId::new(timestamp_ms, sequence).expect("in range");
        prop_assert_eq!(id.timestamp_ms(), timestamp_ms);
        prop_assert_eq!(id.sequence(), sequence);
        prop_assert_eq!(TimeSortableId::from_raw(id.get()), Some(id));
        prop_assert_eq!(
            TimeSortableId::from_unix_ms(id.unix_ms(), sequence),
            Ok(id)
        );
    }

    #[test]
    fn ids_order_by_time_then_sequence(a in id_parts(), b in id_parts()) {
        let left = TimeSortableId::new(a.0, a.1).expect("in range");
        let right = TimeSortableId::new(b.0, b.1).expect("in range");
        prop_assert_eq!(left.cmp(&right), a.cmp(&b));
        prop_assert_eq!(left.get().cmp(&right.get()), a.cmp(&b));
    }

    #[test]
    fn every_non_zero_integer_is_an_id(raw in 1_u64..) {
        let id = TimeSortableId::from_raw(raw).expect("not zero");
        prop_assert_eq!(TimeSortableId::new(id.timestamp_ms(), id.sequence()), Ok(id));
    }
}
