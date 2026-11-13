//! Control messages on the wire: golden bytes that pin field numbers and
//! types at the level of bytes, and what absent and unknown values mean.

mod common;

use common::hex;
use parties_proto::prost::Message;
use parties_proto::v2::{
    AuthOk, AuthRefusal, AuthRefusalReason, AuthResult, ClientFrame, ClientHandshake, Event, Hello,
    HierarchyRule, JoinVoice, LimitCaps, Overwrite, PublishTrackResult, Response, ServerFrame,
    ServerHandshake, SetVoiceState, Status, StatusCode, Track, TrackKind, TrackUnpublished,
    VideoCodec, auth_result, client_frame, event, overwrite, response,
};

#[track_caller]
fn assert_golden<M: Message + Default + PartialEq + std::fmt::Debug>(message: &M, golden: &[u8]) {
    assert_eq!(message.encode_to_vec(), golden, "encoding of {message:?}");
    assert_eq!(
        &M::decode(golden).expect("the golden bytes decode"),
        message
    );
}

#[test]
fn hello_golden_bytes() {
    let hello = ClientHandshake::from(Hello {
        protocol_minor: 0,
        features: Vec::new(),
        client_name: "parties-desktop".to_owned(),
        client_version: "2.0.0".to_owned(),
        platform: "windows".to_owned(),
    });
    // ClientHandshake.hello = 1; Hello.client_name = 3, .client_version = 4,
    // .platform = 5. The minor version 0 and the empty list are not sent.
    let golden = [
        &hex("0a 21 1a 0f")[..],
        b"parties-desktop",
        &hex("22 05"),
        b"2.0.0",
        &hex("2a 07"),
        b"windows",
    ]
    .concat();
    assert_golden(&hello, &golden);

    let with_features = Hello {
        protocol_minor: 3,
        features: vec!["resume".to_owned(), "simulcast".to_owned()],
        ..Hello::default()
    };
    let golden = [
        &hex("08 03 12 06")[..],
        b"resume",
        &hex("12 09"),
        b"simulcast",
    ]
    .concat();
    assert_golden(&with_features, &golden);
}

#[test]
fn auth_result_golden_bytes() {
    let ok = ServerHandshake::from(AuthResult {
        outcome: Some(auth_result::Outcome::Ok(AuthOk {
            user_id: 1,
            session_id: 2,
            picture_wanted: true,
        })),
    });
    // ServerHandshake.auth_result = 2; AuthResult.ok = 1.
    assert_golden(&ok, &hex("12 08 0a 06 08 01 10 02 18 01"));

    let banned = AuthResult {
        outcome: Some(auth_result::Outcome::Refusal(AuthRefusal {
            reason: AuthRefusalReason::Banned.into(),
            ban_expires_at_ms: 1000,
        })),
    };
    // AuthResult.refusal = 2; reason 4, expiry 1000.
    assert_golden(&banned, &hex("12 05 08 04 10 e8 07"));
}

#[test]
fn status_golden_bytes() {
    let denied = Status {
        code: StatusCode::PermissionDenied.into(),
        message: String::new(),
        retry_after_ms: 0,
        // Speak, bit 33.
        missing_permissions: 1 << 33,
        rule: HierarchyRule::H5.into(),
    };
    assert_golden(&denied, &hex("08 01 20 8080808020 28 05"));

    let rate_limited = Status {
        code: StatusCode::RateLimited.into(),
        message: "slow down".to_owned(),
        retry_after_ms: 250,
        ..Status::default()
    };
    let golden = [&hex("08 03 12 09")[..], b"slow down", &hex("18 fa 01")].concat();
    assert_golden(&rate_limited, &golden);
}

#[test]
fn client_frame_golden_bytes() {
    let join = ClientFrame {
        request_id: 7,
        reason: String::new(),
        body: Some(client_frame::Body::JoinVoice(JoinVoice {
            channel_id: 300,
            decode_codecs: vec![VideoCodec::Av1.into(), VideoCodec::H264.into()],
            self_muted: true,
            self_deafened: false,
        })),
    };
    // request_id = 1; join_voice = 32, whose key takes two bytes (82 02);
    // the codecs are packed.
    assert_golden(&join, &hex("08 07 82 02 09 08 ac 02 12 02 01 03 18 01"));

    // An optional field that is set to its default value is sent, so that
    // "unmute" differs from "leave as it is".
    let unmute = ClientFrame {
        request_id: 8,
        reason: String::new(),
        body: Some(client_frame::Body::SetVoiceState(SetVoiceState {
            self_muted: Some(false),
            self_deafened: None,
        })),
    };
    // set_voice_state = 34: key 92 02.
    assert_golden(&unmute, &hex("08 08 92 02 02 08 00"));

    let with_reason = ClientFrame {
        request_id: 9,
        reason: "spam".to_owned(),
        body: None,
    };
    assert_golden(&with_reason, &[&hex("08 09 12 04")[..], b"spam"].concat());
}

#[test]
fn server_frame_golden_bytes() {
    let event = ServerFrame::from(Event {
        seq: 42,
        body: Some(event::Body::TrackUnpublished(TrackUnpublished {
            track_id: 5,
        })),
    });
    // ServerFrame.event = 2; Event.seq = 1; track_unpublished = 35 (9a 02).
    assert_golden(&event, &hex("12 07 08 2a 9a 02 02 08 05"));

    let response = ServerFrame::from(Response {
        request_id: 7,
        status: None,
        payload: Some(response::Payload::PublishTrack(PublishTrackResult {
            track: Some(Track {
                id: 9,
                kind: TrackKind::Voice.into(),
                ..Track::default()
            }),
        })),
    });
    // ServerFrame.response = 1; the result has the number of its request,
    // publish_track = 35.
    assert_golden(&response, &hex("0a 0b 08 07 9a 02 06 0a 04 08 09 10 01"));
}

#[test]
fn state_types_golden_bytes() {
    let overwrite = Overwrite {
        target: Some(overwrite::Target::RoleId(5)),
        allow: 1,
        deny: 2,
    };
    assert_golden(&overwrite, &hex("08 05 18 01 20 02"));

    let for_member = Overwrite {
        target: Some(overwrite::Target::UserId(5)),
        allow: 0,
        deny: 1 << 33,
    };
    assert_golden(&for_member, &hex("10 05 20 8080808020"));

    // A cap of 0 is a cap; a cap that is not set is no cap.
    let caps = LimitCaps {
        max_video_bitrate: Some(0),
        max_published_video_tracks: Some(1),
        ..LimitCaps::default()
    };
    assert_golden(&caps, &hex("08 00 38 01"));
    assert_golden(&LimitCaps::default(), &[]);
}

#[test]
fn a_response_without_a_status_is_a_success() {
    let response = Response {
        request_id: 1,
        status: None,
        payload: None,
    };
    assert_eq!(response.status.unwrap_or_default().code(), StatusCode::Ok);
    assert_eq!(StatusCode::Ok as i32, 0);
    assert_eq!(Status::default().code(), StatusCode::Ok);
}

#[test]
fn an_enum_value_from_a_newer_version_is_kept_as_a_number() {
    // Status.code = 77: no such code in this version.
    let status = Status::decode(&hex("08 4d")[..]).expect("decodes");
    assert_eq!(status.code, 77);
    // The typed conversion refuses it, which is what request validation
    // relies on; the accessor falls back to the default.
    assert!(StatusCode::try_from(status.code).is_err());
    assert_eq!(status.code(), StatusCode::Ok);
    // Encoding again keeps the number.
    assert_eq!(status.encode_to_vec(), hex("08 4d"));
}

#[test]
fn a_oneof_member_from_a_newer_version_reads_as_no_member() {
    // An Event with seq 3 and a body in field 900.
    let event = Event::decode(&hex("08 03 a2 38 01 00")[..]).expect("decodes");
    assert_eq!(event, Event { seq: 3, body: None });
}
