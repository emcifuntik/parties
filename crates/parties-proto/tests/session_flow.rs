//! One session, end to end, in the types of this crate and nothing else:
//! connect, authenticate, receive the state, join a voice channel, send and
//! receive audio and video. There is no network here: the two "streams" are
//! byte buffers. The test shows that the schema of this pass is enough for
//! that path, and how the pieces fit.

mod common;

use bytes::{Bytes, BytesMut};
use common::fixtures;
use parties_proto::frame::{FrameCodec, FrameError};
use parties_proto::ids::TrackId;
use parties_proto::limits::MAX_AUTH_ATTEMPTS;
use parties_proto::media::{
    AudioFlags, AudioHeader, Datagram, KeyframeReason, KeyframeRequest, VideoCodecId, VideoFlags,
    VideoFrameHeader, VideoLayer, VideoStreamHeader,
};
use parties_proto::prost::Message;
use parties_proto::snapshot::ReadyAssembler;
use parties_proto::stream::StreamType;
use parties_proto::v2::{
    AuthOk, AuthRefusal, AuthRefusalReason, AuthResult, Authenticate, ClientFrame, ClientHandshake,
    Event, Hello, HelloAck, IdentityKey, JoinPolicy, JoinRequirements, JoinVoice, KeyAlgorithm,
    PublishTrack, PublishTrackResult, Response, ServerFrame, ServerHandshake, Subscribe, Track,
    TrackKind, TrackPublished, VideoCodec, VoiceState, VoiceStateUpdated, auth_result,
    authenticate, client_frame, client_handshake, event, response, server_frame, server_handshake,
};
use parties_proto::version;

/// One direction of the control stream: what one side wrote and the other
/// has not read yet.
#[derive(Default)]
struct Pipe {
    bytes: BytesMut,
}

impl Pipe {
    fn send<M: Message>(&mut self, codec: FrameCodec, message: &M) {
        codec
            .encode(message, &mut self.bytes)
            .expect("the message fits its frame");
    }

    fn receive<M: Message + Default>(&mut self, codec: FrameCodec) -> M {
        codec
            .decode_message(&mut self.bytes)
            .expect("a well-formed frame")
            .expect("a complete frame")
    }
}

fn key(seed: u8) -> Bytes {
    Bytes::from(vec![seed; 32])
}

fn authenticate(join_code: Option<&str>) -> ClientHandshake {
    Authenticate {
        user_key: Some(IdentityKey {
            algorithm: KeyAlgorithm::Ed25519.into(),
            public_key: key(7),
        }),
        signature: Bytes::from(vec![9; 64]),
        webauthn: None,
        identity_chain: Vec::new(),
        identity_level_counter: 123_456,
        join_credential: join_code
            .map(|code| authenticate::JoinCredential::JoinCode(code.to_owned())),
        display_name: "Mila".to_owned(),
        picture_sha512: fixtures::picture_hash(2),
    }
    .into()
}

#[test]
fn connect_authenticate_join_voice_and_exchange_media() {
    assert_eq!(version::ALPN, b"parties/2");

    // --- The client opens the control stream: its type, then frames. ---
    let mut to_server = Pipe::default();
    let mut to_client = Pipe::default();
    StreamType::Control.encode(&mut to_server.bytes);

    let (stream_type, len) = StreamType::decode(&to_server.bytes)
        .expect("a known type")
        .expect("the type has arrived");
    assert_eq!(stream_type, StreamType::Control);
    assert!(stream_type.allowed_before_authentication());
    let _ = to_server.bytes.split_to(len);

    // --- Handshake, with the limit of an unauthenticated connection. ---
    let mut server_reads = FrameCodec::CONTROL_PRE_AUTH;
    let mut client_reads = FrameCodec::CONTROL_PRE_AUTH;

    // The client already trusts the server key, so it sends `Authenticate`
    // right behind `Hello` (section 4.2). It has no join code yet.
    let hello = Hello {
        protocol_minor: version::PROTOCOL_MINOR,
        features: version::FEATURES
            .iter()
            .map(|&feature| feature.to_owned())
            .collect(),
        client_name: "parties-desktop".to_owned(),
        client_version: "2.0.0".to_owned(),
        platform: "windows".to_owned(),
    };
    to_server.send(server_reads, &ClientHandshake::from(hello.clone()));
    to_server.send(server_reads, &authenticate(None));

    let Some(client_handshake::Kind::Hello(received)) =
        to_server.receive::<ClientHandshake>(server_reads).kind
    else {
        panic!("the first frame is Hello");
    };
    assert_eq!(received, hello);
    to_client.send(
        client_reads,
        &ServerHandshake::from(HelloAck {
            protocol_minor: version::negotiate_minor(
                received.protocol_minor,
                version::PROTOCOL_MINOR,
            ),
            features: Vec::new(),
            server_identity: key(1),
            server_proof: Bytes::from(vec![2; 64]),
            join: Some(JoinRequirements {
                policy: JoinPolicy::JoinCode.into(),
                password_kdf: None,
                min_identity_level: 8,
            }),
            server_name: "Friday Night".to_owned(),
        }),
    );

    // The identity is not a member: the first attempt is refused.
    let mut attempts = 0;
    let Some(client_handshake::Kind::Authenticate(first)) =
        to_server.receive::<ClientHandshake>(server_reads).kind
    else {
        panic!("the second frame is Authenticate");
    };
    attempts += 1;
    assert_eq!(first.join_credential, None);
    to_client.send(
        client_reads,
        &ServerHandshake::from(AuthResult {
            outcome: Some(auth_result::Outcome::Refusal(AuthRefusal {
                reason: AuthRefusalReason::JoinRequired.into(),
                ban_expires_at_ms: 0,
            })),
        }),
    );

    let Some(server_handshake::Kind::HelloAck(hello_ack)) =
        to_client.receive::<ServerHandshake>(client_reads).kind
    else {
        panic!("the server answers Hello first");
    };
    assert_eq!(
        hello_ack.join.expect("join requirements").policy(),
        JoinPolicy::JoinCode
    );
    let Some(server_handshake::Kind::AuthResult(refused)) =
        to_client.receive::<ServerHandshake>(client_reads).kind
    else {
        panic!("then the login");
    };
    let Some(auth_result::Outcome::Refusal(refusal)) = refused.outcome else {
        panic!("the first attempt is refused");
    };
    assert_eq!(refusal.reason(), AuthRefusalReason::JoinRequired);

    // The second attempt carries a join code and is accepted.
    to_server.send(server_reads, &authenticate(Some("K7QW2M9XTR")));
    let Some(client_handshake::Kind::Authenticate(second)) =
        to_server.receive::<ClientHandshake>(server_reads).kind
    else {
        panic!("another Authenticate");
    };
    attempts += 1;
    assert!(attempts <= MAX_AUTH_ATTEMPTS);
    assert_eq!(
        second.join_credential,
        Some(authenticate::JoinCredential::JoinCode(
            "K7QW2M9XTR".to_owned()
        ))
    );
    let snapshot = fixtures::ready();
    let me = snapshot.me.clone().expect("own record");
    to_client.send(
        client_reads,
        &ServerHandshake::from(AuthResult {
            outcome: Some(auth_result::Outcome::Ok(AuthOk {
                user_id: me.user_id,
                session_id: 0x5E55_0002,
                picture_wanted: true,
            })),
        }),
    );
    // Authentication succeeded: both sides raise the frame limit, and the
    // snapshot may use it.
    server_reads.set_max_len(FrameCodec::CONTROL.max_len());
    to_client.send(
        FrameCodec::CONTROL,
        &ServerHandshake::from(snapshot.clone()),
    );

    let Some(server_handshake::Kind::AuthResult(accepted)) =
        to_client.receive::<ServerHandshake>(client_reads).kind
    else {
        panic!("the answer to the second attempt");
    };
    let Some(auth_result::Outcome::Ok(ok)) = accepted.outcome else {
        panic!("the second attempt is accepted");
    };
    assert!(ok.picture_wanted);
    client_reads.set_max_len(FrameCodec::CONTROL.max_len());

    let mut assembler = ReadyAssembler::new();
    let ready = loop {
        let Some(server_handshake::Kind::Ready(part)) =
            to_client.receive::<ServerHandshake>(client_reads).kind
        else {
            panic!("Ready follows a successful login");
        };
        if let Some(ready) = assembler.push(part).expect("within the frame limit") {
            break ready;
        }
    };
    assert_eq!(ready, snapshot);

    // --- The envelope: join a voice channel, publish the microphone. ---
    let lobby = ready
        .channels
        .iter()
        .find(|channel| channel.name == "Lobby")
        .expect("the Lobby channel");
    to_server.send(
        server_reads,
        &ClientFrame {
            request_id: 1,
            reason: String::new(),
            body: Some(client_frame::Body::JoinVoice(JoinVoice {
                channel_id: lobby.id,
                decode_codecs: vec![VideoCodec::Av1.into(), VideoCodec::H264.into()],
                self_muted: false,
                self_deafened: false,
            })),
        },
    );
    // Requests may be sent back to back: the stream keeps their order.
    to_server.send(
        server_reads,
        &ClientFrame {
            request_id: 2,
            reason: String::new(),
            body: Some(client_frame::Body::PublishTrack(PublishTrack {
                kind: TrackKind::Voice.into(),
                ..PublishTrack::default()
            })),
        },
    );

    let join: ClientFrame = to_server.receive(server_reads);
    let Some(client_frame::Body::JoinVoice(join_voice)) = join.body else {
        panic!("request 1 is JoinVoice");
    };
    // The response comes first, then the event the request caused.
    to_client.send(
        client_reads,
        &ServerFrame::from(Response {
            request_id: join.request_id,
            status: None,
            payload: None,
        }),
    );
    let voice_state = VoiceState {
        session_id: ok.session_id,
        user_id: ok.user_id,
        channel_id: join_voice.channel_id,
        decode_codecs: join_voice.decode_codecs,
        ..VoiceState::default()
    };
    to_client.send(
        client_reads,
        &ServerFrame::from(Event {
            seq: ready.seq + 1,
            body: Some(event::Body::VoiceStateUpdated(VoiceStateUpdated {
                state: Some(voice_state.clone()),
            })),
        }),
    );

    let publish: ClientFrame = to_server.receive(server_reads);
    let Some(client_frame::Body::PublishTrack(publish_track)) = publish.body else {
        panic!("request 2 is PublishTrack");
    };
    let voice_track = Track {
        id: 21,
        kind: publish_track.kind,
        session_id: ok.session_id,
        user_id: ok.user_id,
        channel_id: lobby.id,
        ..Track::default()
    };
    to_client.send(
        client_reads,
        &ServerFrame::from(Response {
            request_id: publish.request_id,
            status: None,
            payload: Some(response::Payload::PublishTrack(PublishTrackResult {
                track: Some(voice_track),
            })),
        }),
    );
    to_client.send(
        client_reads,
        &ServerFrame::from(Event {
            seq: ready.seq + 2,
            body: Some(event::Body::TrackPublished(TrackPublished {
                track: Some(voice_track),
            })),
        }),
    );

    let mut last_seq = ready.seq;
    let mut my_track = None;
    for _ in 0..4 {
        match to_client.receive::<ServerFrame>(client_reads).kind {
            Some(server_frame::Kind::Response(response)) => {
                assert_eq!(response.status.unwrap_or_default().code, 0, "OK");
                if let Some(response::Payload::PublishTrack(result)) = response.payload {
                    assert_eq!(response.request_id, 2);
                    my_track = result.track.map(|track| TrackId(track.id));
                }
            }
            Some(server_frame::Kind::Event(event)) => {
                assert!(event.seq > last_seq, "events are strictly increasing");
                last_seq = event.seq;
            }
            None => panic!("a frame of a kind this version does not know"),
        }
    }
    let my_track = my_track.expect("the server assigned a track id");
    assert!(to_client.bytes.is_empty() && to_server.bytes.is_empty());

    // --- Audio: one datagram, forwarded as the same bytes. ---
    let opus = [0x5A; 60];
    let mut datagram = BytesMut::with_capacity(AudioHeader::MAX_LEN + opus.len());
    AudioHeader {
        track: my_track,
        seq: 1,
        timestamp: 960,
        flags: AudioFlags::EMPTY,
        level: 35,
    }
    .encode(&mut datagram);
    datagram.extend_from_slice(&opus);
    let datagram = datagram.freeze();

    // The server reads the header, keeps the buffer, and hands every
    // recipient a reference to it.
    let Ok(Datagram::Audio(at_server)) = Datagram::decode(&datagram) else {
        panic!("an audio datagram");
    };
    assert_eq!(at_server.header.track, my_track);
    assert_eq!(at_server.header.level_dbov(), 35);
    let forwarded = datagram.clone();
    assert_eq!(forwarded.as_ptr(), datagram.as_ptr(), "no copy");
    let Ok(Datagram::Audio(at_listener)) = Datagram::decode(&forwarded) else {
        panic!("an audio datagram");
    };
    assert_eq!(at_listener.payload, &opus);

    // --- Video: subscribe to the owner's screen share, receive a frame. ---
    let screen = ready
        .tracks
        .iter()
        .find(|track| track.kind() == TrackKind::Screen)
        .expect("a screen share");
    to_server.send(
        server_reads,
        &ClientFrame {
            request_id: 3,
            reason: String::new(),
            body: Some(client_frame::Body::Subscribe(Subscribe {
                track_id: screen.id,
            })),
        },
    );
    let subscribe: ClientFrame = to_server.receive(server_reads);
    assert!(matches!(
        subscribe.body,
        Some(client_frame::Body::Subscribe(Subscribe { track_id })) if track_id == screen.id
    ));

    // The viewer asks for a first keyframe; the request is a datagram.
    let mut request = Vec::new();
    KeyframeRequest {
        track: TrackId(screen.id),
        reason: KeyframeReason::NEW_VIEWER,
    }
    .encode(&mut request);
    assert!(matches!(
        Datagram::decode(&request),
        Ok(Datagram::KeyframeRequest(KeyframeRequest { track, .. })) if track.0 == screen.id
    ));

    // The publisher writes the frame on a stream of its own. The server
    // forwards identical bytes on a new stream to the viewer.
    let encoded_frame = vec![0xC3; 5000];
    let header = VideoStreamHeader {
        track: TrackId(screen.id),
        frame: VideoFrameHeader {
            frame_seq: 1,
            capture_ts_ms: 16,
            flags: VideoFlags::KEYFRAME,
            layer: VideoLayer::BASE,
            codec: VideoCodecId::from(screen.codec()),
            width: 2560,
            height: 1440,
        },
    };
    let mut stream = BytesMut::with_capacity(header.encoded_len() + encoded_frame.len());
    header.encode(&mut stream);
    stream.extend_from_slice(&encoded_frame);
    let stream = stream.freeze();

    // The viewer routes the stream by its type, then reads the header.
    let (stream_type, type_len) = StreamType::decode(&stream)
        .expect("known")
        .expect("arrived");
    assert_eq!(stream_type, StreamType::VideoFrame);
    let (received, payload_at) = VideoStreamHeader::decode_after_type(&stream[type_len..])
        .expect("a well-formed header")
        .expect("a complete header");
    assert_eq!(received, header);
    assert!(received.frame.is_keyframe());
    assert_eq!(received.frame.codec.known(), Some(VideoCodec::Av1));
    let payload = stream.slice(type_len + payload_at..);
    assert_eq!(payload, encoded_frame);

    // --- A frame over the limit ends the session. ---
    let mut too_long = BytesMut::new();
    let oversized = ClientFrame {
        request_id: 4,
        reason: "x".repeat(70_000),
        body: None,
    };
    assert!(matches!(
        FrameCodec::CONTROL.encode(&oversized, &mut too_long),
        Err(FrameError::TooLong { max: 65_536, .. })
    ));
}
