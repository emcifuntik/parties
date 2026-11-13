//! The state snapshot (section 5.3): a realistic `Ready` round-trips, fits
//! its frame, and a large one travels in several frames.

mod common;

use bytes::BytesMut;
use common::fixtures::{self, bit, mask};
use parties_proto::frame::{FrameCodec, FrameError};
use parties_proto::limits::{
    MAX_CONTROL_FRAME_LEN, MAX_MEMBERS_IN_READY, MAX_READY_FRAMES, MAX_READY_ITEMS,
};
use parties_proto::prost::Message;
use parties_proto::snapshot::{ReadyAssembler, SnapshotError};
use parties_proto::v2::{
    ChannelKind, ChannelPermissions, JoinPolicy, Member, Presence, Ready, Role, ServerHandshake,
    TrackKind, VideoCodec, overwrite, server_handshake,
};

/// Sends `frames` through the control stream's codec and reads them back.
fn through_the_wire(frames: &[ServerHandshake]) -> Vec<ServerHandshake> {
    let codec = FrameCodec::CONTROL;
    let mut wire = BytesMut::new();
    for frame in frames {
        codec.encode(frame, &mut wire).expect("the frame fits");
    }
    let mut received = Vec::new();
    while let Some(frame) = codec.decode_message(&mut wire).expect("well-formed") {
        received.push(frame);
    }
    assert!(wire.is_empty());
    received
}

fn ready_of(frame: ServerHandshake) -> Ready {
    match frame.kind {
        Some(server_handshake::Kind::Ready(ready)) => ready,
        other => panic!("not a Ready frame: {other:?}"),
    }
}

#[test]
fn a_realistic_snapshot_round_trips() {
    let ready = fixtures::ready();
    let received = through_the_wire(&[ready.clone().into()]);
    assert_eq!(received.len(), 1);
    assert_eq!(ready_of(received[0].clone()), ready);

    // Byte for byte, too: decoding and encoding again gives the same frame.
    let encoded = ready.encode_to_vec();
    let decoded = Ready::decode(encoded.as_slice()).expect("decodes");
    assert_eq!(decoded.encode_to_vec(), encoded);
}

#[test]
fn the_snapshot_reads_back_as_it_was_meant() {
    let encoded = fixtures::ready().encode_to_vec();
    let ready = Ready::decode(encoded.as_slice()).expect("decodes");

    let settings = ready.settings.as_ref().expect("settings");
    assert_eq!(settings.name, "Friday Night");
    assert_eq!(settings.join_policy(), JoinPolicy::JoinCode);
    let caps = settings.limit_caps.expect("server-wide caps");
    assert_eq!(caps.max_video_bitrate, Some(12_000));
    assert_eq!(
        caps.max_video_fps, None,
        "a limit that is not set is not capped"
    );
    let policy = settings.new_members.expect("new-member policy");
    assert_eq!(policy.period_ms, 600_000);
    assert_ne!(policy.restricted & mask(&[bit::SPEAK]), 0);

    // Roles: positions are unique, `@everyone` is at 0 and is the one the
    // snapshot names.
    let everyone = ready
        .roles
        .iter()
        .find(|role| role.id == ready.everyone_role_id)
        .expect("the @everyone role is among the roles");
    assert_eq!(everyone.position, 0);
    assert_eq!(everyone.permissions, fixtures::everyone_permissions());
    assert_eq!(
        everyone.limits.expect("default limits").max_video_height,
        1080
    );
    let mut positions: Vec<_> = ready.roles.iter().map(|role| role.position).collect();
    positions.dedup();
    assert_eq!(positions, [0, 1, 2, 3]);
    let admin = ready.roles.last().expect("four roles");
    assert_eq!(admin.permissions, 1 << 62);
    assert!(admin.requires_hardware_key);

    // The moderated channel: a role overwrite that denies, one that allows,
    // and one for a single member.
    let stage = ready
        .channels
        .iter()
        .find(|channel| channel.name == "Stage")
        .expect("the Stage channel");
    assert_eq!(stage.kind(), ChannelKind::Voice);
    assert_ne!(stage.parent_id, 0);
    let targets: Vec<_> = stage.overwrites.iter().map(|entry| entry.target).collect();
    assert!(matches!(
        targets[..],
        [
            Some(overwrite::Target::RoleId(_)),
            Some(overwrite::Target::RoleId(_)),
            Some(overwrite::Target::UserId(_)),
        ]
    ));
    assert_eq!(stage.overwrites[0].deny, mask(&[bit::SPEAK]));
    assert_eq!(stage.overwrites[1].allow, mask(&[bit::SPEAK]));
    for entry in &stage.overwrites {
        assert_eq!(entry.allow & entry.deny, 0, "the masks never overlap");
    }
    assert_eq!(
        stage.limit_caps.expect("channel caps").max_video_height,
        Some(720)
    );

    // The member's own record, permissions and limits.
    let me = ready.me.as_ref().expect("own record");
    assert_eq!(me.nickname, "Mila (mod)");
    assert_eq!(me.picture_sha512.len(), 64);
    assert!(ready.members.contains(me));
    assert_ne!(
        ready.server_permissions & mask(&[bit::KICK_MEMBERS]),
        0,
        "a moderator may kick"
    );
    // One entry of effective permissions for every channel sent.
    let channels: Vec<_> = ready.channels.iter().map(|channel| channel.id).collect();
    let with_permissions: Vec<_> = ready
        .channel_permissions
        .iter()
        .map(|entry| entry.channel_id)
        .collect();
    assert_eq!(with_permissions, channels);
    assert_eq!(ready.limits.expect("limits").max_video_bitrate, 12_000);

    // Members: every presence, a timeout, a last-seen time.
    let presences: Vec<_> = ready.members.iter().map(Member::presence).collect();
    assert!(presences.contains(&Presence::Idle));
    assert!(presences.contains(&Presence::DoNotDisturb));
    let offline = ready
        .members
        .iter()
        .find(|member| member.presence() == Presence::Offline)
        .expect("an offline member");
    assert_ne!(offline.last_seen_at_ms, 0);
    assert!(
        ready
            .members
            .iter()
            .any(|member| member.timeout_until_ms > fixtures::NOW_UNIX_MS)
    );
    assert_eq!(ready.member_count, 6);
    assert!(!ready.members_partial);

    // Voice: every track belongs to a session that is in that channel, and
    // the screen audio points at its screen.
    for track in &ready.tracks {
        assert!(
            ready
                .voice_states
                .iter()
                .any(|state| state.session_id == track.session_id
                    && state.channel_id == track.channel_id),
            "track {} has no voice state",
            track.id
        );
    }
    let screen_audio = ready
        .tracks
        .iter()
        .find(|track| track.kind() == TrackKind::ScreenAudio)
        .expect("a screen audio track");
    let screen = ready
        .tracks
        .iter()
        .find(|track| track.id == screen_audio.linked_track_id)
        .expect("its screen");
    assert_eq!(screen.kind(), TrackKind::Screen);
    assert_eq!(screen.codec(), VideoCodec::Av1);
    assert_eq!((screen.width, screen.height), (2560, 1440));
    assert!(ready.voice_states.iter().any(|state| state.speak_granted));
}

#[test]
fn a_small_community_fits_one_frame_with_room_to_spare() {
    let len = fixtures::ready().encoded_len();
    assert!(len < 4096, "the fixture encodes to {len} bytes");
    assert!(len < MAX_CONTROL_FRAME_LEN / 8);
}

/// Splits a snapshot the way a server does: everything but the members in
/// the first frame, then the members in groups.
fn split(ready: &Ready, members_per_frame: usize) -> Vec<Ready> {
    let mut parts = vec![Ready {
        members: Vec::new(),
        ..ready.clone()
    }];
    for members in ready.members.chunks(members_per_frame) {
        parts.push(Ready {
            members: members.to_vec(),
            ..Ready::default()
        });
    }
    let last = parts.len() - 1;
    for (index, part) in parts.iter_mut().enumerate() {
        part.more = index != last;
    }
    parts
}

fn large_snapshot() -> Ready {
    let members =
        fixtures::member_directory(u32::try_from(MAX_MEMBERS_IN_READY).expect("a thousand"));
    Ready {
        member_count: 1000,
        members,
        ..fixtures::ready()
    }
}

#[test]
fn the_directory_of_a_thousand_members_does_not_fit_one_frame() {
    let ready = large_snapshot();
    assert_eq!(ready.members.len(), 1000);
    let len = ready.encoded_len();
    assert!(len > MAX_CONTROL_FRAME_LEN, "{len} bytes");
    let mut wire = BytesMut::new();
    let frame = ServerHandshake::from(ready);
    assert!(matches!(
        FrameCodec::CONTROL.encode(&frame, &mut wire),
        Err(FrameError::TooLong { max: 65_536, .. })
    ));
}

#[test]
fn a_large_snapshot_travels_in_several_frames() {
    let ready = large_snapshot();
    let parts = split(&ready, 250);
    assert_eq!(parts.len(), 5);
    assert!(parts.len() <= MAX_READY_FRAMES);
    for part in &parts {
        assert!(part.encoded_len() <= MAX_CONTROL_FRAME_LEN - 16);
    }

    let frames: Vec<ServerHandshake> = parts.into_iter().map(Into::into).collect();
    let mut assembler = ReadyAssembler::new();
    let mut assembled = None;
    for frame in through_the_wire(&frames) {
        assert!(assembled.is_none(), "nothing follows the last frame");
        assembled = assembler
            .push(ready_of(frame))
            .expect("within the frame limit");
    }
    assert_eq!(assembled, Some(ready));
}

#[test]
fn assembling_agrees_with_merging_the_encoded_frames() {
    // Protobuf defines what it means to decode several encodings of one
    // message in a row: repeated fields are appended. The assembler gives
    // the same result.
    let ready = large_snapshot();
    let parts = split(&ready, 300);

    let mut merged = Ready::default();
    for part in &parts {
        merged
            .merge(part.encode_to_vec().as_slice())
            .expect("decodes");
    }
    // The first frame set `more`; the last frame, like every encoding of
    // `false`, does not clear it.
    assert!(merged.more);
    merged.more = false;

    let mut assembler = ReadyAssembler::new();
    let mut assembled = None;
    for part in parts {
        assert!(assembled.is_none(), "nothing follows the last frame");
        assembled = assembler.push(part).expect("within the frame limit");
    }
    assert_eq!(assembled, Some(merged));
}

#[test]
fn a_snapshot_in_one_frame_needs_no_assembly() {
    let ready = fixtures::ready();
    let mut assembler = ReadyAssembler::new();
    assert_eq!(assembler.push(ready.clone()), Ok(Some(ready.clone())));
    // The assembler starts over: a second snapshot is not mixed with the
    // first.
    assert_eq!(assembler.push(ready.clone()), Ok(Some(ready)));
}

#[test]
fn a_snapshot_of_too_many_frames_is_refused() {
    let part = |more| Ready {
        more,
        members: fixtures::member_directory(1),
        ..Ready::default()
    };
    let mut assembler = ReadyAssembler::new();
    for _ in 0..MAX_READY_FRAMES - 1 {
        assert_eq!(assembler.push(part(true)), Ok(None));
    }
    // The sixteenth frame may still complete the snapshot.
    let mut exactly = assembler.clone();
    let ready = exactly
        .push(part(false))
        .expect("sixteen frames")
        .expect("complete");
    assert_eq!(ready.members.len(), MAX_READY_FRAMES);

    // A seventeenth frame is one too many, whatever it says.
    assert_eq!(assembler.push(part(true)), Ok(None));
    assert_eq!(
        assembler.push(part(false)),
        Err(SnapshotError::TooManyFrames)
    );
    assert_eq!(
        SnapshotError::TooManyFrames.to_string(),
        "the state snapshot has more than 16 frames"
    );
    // The assembler dropped what it had collected.
    assert_eq!(
        assembler
            .push(part(false))
            .map(|ready| ready.map(|r| r.members.len())),
        Ok(Some(1))
    );
}

#[test]
fn a_snapshot_of_too_many_entries_is_refused() {
    // On the wire an entry can be as short as two bytes: a tag and a length
    // of zero. Such a frame of 64 KiB holds 32,768 entries, each of which
    // costs a client far more memory than it cost the server to send.
    let cheap = Ready {
        more: true,
        members: vec![Member::default(); MAX_CONTROL_FRAME_LEN / 2 - 2],
        ..Ready::default()
    };
    assert!(cheap.encoded_len() <= MAX_CONTROL_FRAME_LEN);
    assert_eq!(cheap.encoded_len(), 2 * cheap.members.len() + 2);
    let mut assembler = ReadyAssembler::new();
    assert_eq!(assembler.push(cheap), Err(SnapshotError::TooManyItems));
    assert_eq!(
        SnapshotError::TooManyItems.to_string(),
        "the state snapshot has more than 16384 entries"
    );

    // The limit counts every repeated field, across the frames.
    assert_eq!(MAX_READY_ITEMS, 16_384);
    let quarter = MAX_READY_ITEMS / 4;
    let roles = Ready {
        more: true,
        roles: vec![Role::default(); quarter],
        ..Ready::default()
    };
    let members = Ready {
        more: true,
        members: vec![Member::default(); quarter],
        ..Ready::default()
    };
    let permissions = |more, extra| Ready {
        more,
        channel_permissions: vec![ChannelPermissions::default(); 2 * quarter + extra],
        ..Ready::default()
    };

    let mut assembler = ReadyAssembler::new();
    assert_eq!(assembler.push(roles.clone()), Ok(None));
    assert_eq!(assembler.push(members.clone()), Ok(None));
    let ready = assembler
        .push(permissions(false, 0))
        .expect("exactly the limit")
        .expect("complete");
    assert_eq!(
        ready.roles.len() + ready.members.len() + ready.channel_permissions.len(),
        MAX_READY_ITEMS
    );

    assert_eq!(assembler.push(roles), Ok(None));
    assert_eq!(assembler.push(members), Ok(None));
    assert_eq!(
        assembler.push(permissions(true, 1)),
        Err(SnapshotError::TooManyItems)
    );
    // The assembler dropped what it had collected.
    assert_eq!(assembler.push(Ready::default()), Ok(Some(Ready::default())));
}

#[test]
fn per_channel_permissions_may_span_frames() {
    // The list grows with the channels, so it is split like them.
    let entry = |channel_id| ChannelPermissions {
        channel_id,
        permissions: 1,
    };
    let first = Ready {
        more: true,
        server_permissions: 7,
        channel_permissions: vec![entry(1), entry(2)],
        ..Ready::default()
    };
    let last = Ready {
        channel_permissions: vec![entry(3)],
        ..Ready::default()
    };
    let mut assembler = ReadyAssembler::new();
    assert_eq!(assembler.push(first), Ok(None));
    let ready = assembler.push(last).expect("two frames").expect("complete");
    assert_eq!(ready.server_permissions, 7);
    assert_eq!(ready.channel_permissions, [entry(1), entry(2), entry(3)]);
}
