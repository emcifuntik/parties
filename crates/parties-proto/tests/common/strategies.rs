//! `proptest` strategies for the generated messages and the media headers.
//!
//! The strategies generate what the wire can carry, not only what a
//! well-behaved peer sends: enum fields hold numbers no version defines,
//! strings are arbitrary Unicode, masks have reserved bits.
//!
//! The strategies of messages are boxed. Unboxed, their types nest so
//! deeply that generating a snapshot overflows the stack of a test thread
//! in a debug build.

use bytes::Bytes;
use parties_proto::ids::TrackId;
use parties_proto::media::{
    AudioFlags, AudioHeader, KeyframeReason, KeyframeRequest, VideoCodecId, VideoFlags,
    VideoFrameHeader, VideoLayer, VideoStreamHeader,
};
use parties_proto::v2::{
    AssignRole, Channel, ChannelPermissions, ClientFrame, CreateJoinCode, Event, JoinVoice,
    LimitCaps, Member, NewMemberPolicy, Overwrite, PermissionsUpdated, PresenceUpdated,
    PublishTrack, PublishTrackResult, Ready, Response, Role, RoleLimits, RoleResult, RoleUpdated,
    ServerFrame, ServerSettings, SetOverwrite, SetVoiceState, Status, Subscribe, Track,
    UpdateChannel, UpdateProfile, UpdateRole, VoiceState, VoiceStateUpdated, client_frame, event,
    overwrite, response,
};
use proptest::collection::vec;
use proptest::option;
use proptest::prelude::*;
use proptest::strategy::Union;
use proptest::test_runner::FileFailurePersistence;

/// The configuration of every property test: at least `cases` cases (the
/// `PROPTEST_CASES` variable can ask for more), and failing inputs saved to
/// `regressions`, a path relative to the crate, so that they can be checked
/// in and are tried first on every later run.
pub fn config(cases: u32, regressions: &'static str) -> ProptestConfig {
    let defaults = ProptestConfig::default();
    ProptestConfig {
        cases: defaults.cases.max(cases),
        failure_persistence: Some(Box::new(FileFailurePersistence::Direct(regressions))),
        ..defaults
    }
}

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

/// A value in the range of a QUIC variable-length integer, with the
/// boundaries of the four lengths drawn often.
pub fn varint_value() -> impl Strategy<Value = u64> {
    prop_oneof![
        0..64_u64,
        64..16_384_u64,
        16_384..1_073_741_824_u64,
        1_073_741_824..=4_611_686_018_427_387_903_u64,
        prop::sample::select(vec![
            0,
            63,
            64,
            16_383,
            16_384,
            1_073_741_823,
            1_073_741_824,
            4_611_686_018_427_387_903_u64,
        ]),
    ]
}

/// A track id, short and long encodings alike.
pub fn track_id() -> impl Strategy<Value = TrackId> {
    prop_oneof![0..64_u32, 64..16_384_u32, any::<u32>()].prop_map(TrackId)
}

fn id() -> impl Strategy<Value = u64> {
    prop_oneof![Just(0), 1..1000_u64, any::<u64>()]
}

fn mask() -> impl Strategy<Value = u64> {
    // Bit 63 is never set in a permission mask.
    any::<u64>().prop_map(|bits| bits >> 1)
}

/// An enum field: the values in use, and some that no version defines.
fn enumeration() -> impl Strategy<Value = i32> {
    prop_oneof![0..6_i32, 6..1000_i32]
}

fn text(max_chars: usize) -> impl Strategy<Value = String> {
    vec(any::<char>(), 0..=max_chars).prop_map(|chars| chars.into_iter().collect())
}

fn blob(max_len: usize) -> impl Strategy<Value = Bytes> {
    vec(any::<u8>(), 0..=max_len).prop_map(Bytes::from)
}

// ---------------------------------------------------------------------------
// Media headers
// ---------------------------------------------------------------------------

pub fn audio_header() -> impl Strategy<Value = AudioHeader> {
    (
        track_id(),
        any::<u16>(),
        any::<u32>(),
        any::<u8>(),
        any::<u8>(),
    )
        .prop_map(|(track, seq, timestamp, flags, level)| AudioHeader {
            track,
            seq,
            timestamp,
            flags: AudioFlags::from_bits_truncate(flags),
            level,
        })
}

pub fn keyframe_request() -> impl Strategy<Value = KeyframeRequest> {
    (track_id(), any::<u8>()).prop_map(|(track, reason)| KeyframeRequest {
        track,
        reason: KeyframeReason(reason),
    })
}

pub fn video_frame_header() -> impl Strategy<Value = VideoFrameHeader> {
    (
        any::<u32>(),
        any::<u32>(),
        any::<u8>(),
        any::<u8>(),
        any::<u8>(),
        any::<u16>(),
        any::<u16>(),
    )
        .prop_map(
            |(frame_seq, capture_ts_ms, flags, layer, codec, width, height)| VideoFrameHeader {
                frame_seq,
                capture_ts_ms,
                flags: VideoFlags::from_bits_truncate(flags),
                layer: VideoLayer(layer),
                codec: VideoCodecId(codec),
                width,
                height,
            },
        )
}

pub fn video_stream_header() -> impl Strategy<Value = VideoStreamHeader> {
    (track_id(), video_frame_header()).prop_map(|(track, frame)| VideoStreamHeader { track, frame })
}

// ---------------------------------------------------------------------------
// State types
// ---------------------------------------------------------------------------

fn role_limits() -> BoxedStrategy<RoleLimits> {
    any::<[u32; 7]>()
        .prop_map(
            |[bitrate, height, fps, aux, upload, quota, tracks]| RoleLimits {
                max_video_bitrate: bitrate,
                max_video_height: height,
                max_video_fps: fps,
                max_aux_audio_bitrate: aux,
                max_upload_size: upload,
                daily_upload_quota: quota,
                max_published_video_tracks: tracks,
            },
        )
        .boxed()
}

fn limit_caps() -> BoxedStrategy<LimitCaps> {
    any::<[Option<u32>; 7]>()
        .prop_map(
            |[bitrate, height, fps, aux, upload, quota, tracks]| LimitCaps {
                max_video_bitrate: bitrate,
                max_video_height: height,
                max_video_fps: fps,
                max_aux_audio_bitrate: aux,
                max_upload_size: upload,
                daily_upload_quota: quota,
                max_published_video_tracks: tracks,
            },
        )
        .boxed()
}

fn overwrite() -> BoxedStrategy<Overwrite> {
    let target = option::of(prop_oneof![
        id().prop_map(overwrite::Target::RoleId),
        id().prop_map(overwrite::Target::UserId),
    ]);
    (target, mask(), mask())
        .prop_map(|(target, allow, deny)| Overwrite {
            target,
            allow,
            deny,
        })
        .boxed()
}

fn role() -> BoxedStrategy<Role> {
    (
        (id(), id(), text(64), any::<u32>(), any::<u32>()),
        (mask(), option::of(role_limits()), any::<[bool; 3]>()),
    )
        .prop_map(
            |((id, revision, name, color, position), (permissions, limits, flags))| Role {
                id,
                revision,
                name,
                color,
                position,
                permissions,
                limits,
                hoisted: flags[0],
                mentionable: flags[1],
                requires_hardware_key: flags[2],
            },
        )
        .boxed()
}

fn channel() -> BoxedStrategy<Channel> {
    (
        (id(), id(), enumeration(), id(), any::<u32>()),
        (text(64), text(80), vec(overwrite(), 0..4)),
        (any::<u32>(), option::of(limit_caps()), any::<u32>()),
    )
        .prop_map(
            |(
                (id, revision, kind, parent_id, position),
                (name, topic, overwrites),
                (user_limit, limit_caps, slow_mode_seconds),
            )| Channel {
                id,
                revision,
                kind,
                parent_id,
                position,
                name,
                topic,
                overwrites,
                user_limit,
                limit_caps,
                slow_mode_seconds,
            },
        )
        .boxed()
}

fn member() -> BoxedStrategy<Member> {
    (
        (id(), text(32), text(32), vec(id(), 0..4)),
        (blob(64), enumeration(), any::<u64>(), any::<u64>()),
        (any::<bool>(), any::<bool>(), any::<u64>()),
    )
        .prop_map(
            |(
                (user_id, display_name, nickname, role_ids),
                (picture_sha512, presence, joined_at_ms, timeout_until_ms),
                (server_muted, server_deafened, last_seen_at_ms),
            )| Member {
                user_id,
                display_name,
                nickname,
                role_ids,
                picture_sha512,
                presence,
                joined_at_ms,
                timeout_until_ms,
                server_muted,
                server_deafened,
                last_seen_at_ms,
            },
        )
        .boxed()
}

fn voice_state() -> BoxedStrategy<VoiceState> {
    (
        id(),
        id(),
        id(),
        any::<[bool; 3]>(),
        vec(enumeration(), 0..4),
    )
        .prop_map(
            |(session_id, user_id, channel_id, flags, decode_codecs)| VoiceState {
                session_id,
                user_id,
                channel_id,
                self_muted: flags[0],
                self_deafened: flags[1],
                speak_granted: flags[2],
                decode_codecs,
            },
        )
        .boxed()
}

fn track() -> BoxedStrategy<Track> {
    (
        (any::<u32>(), enumeration(), id(), id(), id()),
        (any::<u32>(), enumeration(), any::<u32>(), any::<u32>()),
    )
        .prop_map(
            |(
                (id, kind, session_id, user_id, channel_id),
                (linked_track_id, codec, width, height),
            )| {
                Track {
                    id,
                    kind,
                    session_id,
                    user_id,
                    channel_id,
                    linked_track_id,
                    codec,
                    width,
                    height,
                }
            },
        )
        .boxed()
}

fn channel_permissions() -> BoxedStrategy<ChannelPermissions> {
    (id(), mask())
        .prop_map(|(channel_id, permissions)| ChannelPermissions {
            channel_id,
            permissions,
        })
        .boxed()
}

fn server_settings() -> BoxedStrategy<ServerSettings> {
    (
        (id(), text(64), text(120), id(), id(), any::<u32>()),
        (
            enumeration(),
            any::<bool>(),
            any::<u32>(),
            any::<[u32; 3]>(),
        ),
        (
            option::of((any::<u64>(), any::<bool>(), mask())),
            enumeration(),
            option::of(limit_caps()),
        ),
        (any::<u32>(), any::<[bool; 2]>(), any::<[u32; 2]>()),
    )
        .prop_map(
            |(
                (revision, name, description, default_channel_id, afk_channel_id, afk_timeout),
                (join_policy, join_password_set, min_identity_level, maxima),
                (new_members, query_visibility, limit_caps),
                (speaker_limit, flags, join_codes),
            )| ServerSettings {
                revision,
                name,
                description,
                default_channel_id,
                afk_channel_id,
                afk_timeout_seconds: afk_timeout,
                join_policy,
                join_password_set,
                min_identity_level,
                new_members: new_members.map(|(period_ms, require_role, restricted)| {
                    NewMemberPolicy {
                        period_ms,
                        require_role,
                        restricted,
                    }
                }),
                max_members: maxima[0],
                max_sessions: maxima[1],
                max_sessions_per_member: maxima[2],
                query_visibility,
                limit_caps,
                speaker_limit,
                pictures_enabled: flags[0],
                show_last_seen: flags[1],
                max_join_code_lifetime_seconds: join_codes[0],
                max_unused_join_codes_per_member: join_codes[1],
            },
        )
        .boxed()
}

/// A snapshot in one piece: `more` is not set.
pub fn ready() -> BoxedStrategy<Ready> {
    (
        (
            id(),
            option::of(server_settings()),
            id(),
            id(),
            option::of(member()),
        ),
        (
            vec(role(), 0..5),
            vec(channel(), 0..6),
            mask(),
            vec(channel_permissions(), 0..6),
            option::of(role_limits()),
        ),
        (
            vec(voice_state(), 0..6),
            vec(track(), 0..6),
            vec(member(), 0..12),
        ),
        (any::<bool>(), any::<u32>()),
    )
        .prop_map(
            |(
                (seq, settings, owner_id, everyone_role_id, me),
                (roles, channels, server_permissions, channel_permissions, limits),
                (voice_states, tracks, members),
                (members_partial, member_count),
            )| Ready {
                more: false,
                seq,
                settings,
                owner_id,
                everyone_role_id,
                me,
                roles,
                channels,
                server_permissions,
                channel_permissions,
                limits,
                voice_states,
                tracks,
                members,
                members_partial,
                member_count,
            },
        )
        .boxed()
}

// ---------------------------------------------------------------------------
// The envelope
// ---------------------------------------------------------------------------

fn request_body() -> BoxedStrategy<client_frame::Body> {
    let update_profile = (option::of(text(32)), option::of(blob(64)))
        .prop_map(|(display_name, picture_sha512)| {
            client_frame::Body::UpdateProfile(UpdateProfile {
                display_name,
                picture_sha512,
            })
        })
        .boxed();
    let join_voice = (id(), vec(enumeration(), 0..4), any::<[bool; 2]>())
        .prop_map(|(channel_id, decode_codecs, flags)| {
            client_frame::Body::JoinVoice(JoinVoice {
                channel_id,
                decode_codecs,
                self_muted: flags[0],
                self_deafened: flags[1],
            })
        })
        .boxed();
    let set_voice_state = any::<[Option<bool>; 2]>()
        .prop_map(|[self_muted, self_deafened]| {
            client_frame::Body::SetVoiceState(SetVoiceState {
                self_muted,
                self_deafened,
            })
        })
        .boxed();
    let publish_track = (
        enumeration(),
        any::<u32>(),
        enumeration(),
        any::<[u32; 2]>(),
    )
        .prop_map(|(kind, linked_track_id, codec, size)| {
            client_frame::Body::PublishTrack(PublishTrack {
                kind,
                linked_track_id,
                codec,
                width: size[0],
                height: size[1],
            })
        })
        .boxed();
    let subscribe = any::<u32>()
        .prop_map(|track_id| client_frame::Body::Subscribe(Subscribe { track_id }))
        .boxed();
    let update_channel = (
        (id(), id(), option::of(text(64)), option::of(text(80))),
        (
            option::of(id()),
            option::of(any::<u32>()),
            option::of(limit_caps()),
        ),
        option::of(any::<u32>()),
    )
        .prop_map(
            |(
                (channel_id, base_revision, name, topic),
                (parent_id, user_limit, limit_caps),
                slow_mode_seconds,
            )| {
                client_frame::Body::UpdateChannel(UpdateChannel {
                    channel_id,
                    base_revision,
                    name,
                    topic,
                    parent_id,
                    user_limit,
                    limit_caps,
                    slow_mode_seconds,
                })
            },
        )
        .boxed();
    let set_overwrite = (id(), id(), option::of(overwrite()))
        .prop_map(|(channel_id, base_revision, overwrite)| {
            client_frame::Body::SetOverwrite(SetOverwrite {
                channel_id,
                base_revision,
                overwrite,
            })
        })
        .boxed();
    let update_role = (
        (id(), id(), option::of(text(64)), option::of(any::<u32>())),
        (
            option::of(mask()),
            option::of(role_limits()),
            any::<[Option<bool>; 3]>(),
        ),
    )
        .prop_map(
            |((role_id, base_revision, name, color), (permissions, limits, flags))| {
                client_frame::Body::UpdateRole(UpdateRole {
                    role_id,
                    base_revision,
                    name,
                    color,
                    permissions,
                    limits,
                    hoisted: flags[0],
                    mentionable: flags[1],
                    requires_hardware_key: flags[2],
                })
            },
        )
        .boxed();
    let assign_role = (id(), id())
        .prop_map(|(user_id, role_id)| {
            client_frame::Body::AssignRole(AssignRole { user_id, role_id })
        })
        .boxed();
    let create_join_code = (text(40), any::<[u32; 2]>(), vec(id(), 0..4))
        .prop_map(|(label, numbers, role_ids)| {
            client_frame::Body::CreateJoinCode(CreateJoinCode {
                label,
                max_uses: numbers[0],
                lifetime_seconds: numbers[1],
                role_ids,
            })
        })
        .boxed();
    Union::new([
        update_profile,
        join_voice,
        set_voice_state,
        publish_track,
        subscribe,
        update_channel,
        set_overwrite,
        update_role,
        assign_role,
        create_join_code,
    ])
    .boxed()
}

/// A request of one of the areas of this pass.
pub fn client_frame() -> BoxedStrategy<ClientFrame> {
    (any::<u32>(), text(40), option::of(request_body()))
        .prop_map(|(request_id, reason, body)| ClientFrame {
            request_id,
            reason,
            body,
        })
        .boxed()
}

fn status() -> BoxedStrategy<Status> {
    (enumeration(), text(60), any::<u32>(), mask(), enumeration())
        .prop_map(
            |(code, message, retry_after_ms, missing_permissions, rule)| Status {
                code,
                message,
                retry_after_ms,
                missing_permissions,
                rule,
            },
        )
        .boxed()
}

/// A response or an event of one of the areas of this pass.
pub fn server_frame() -> BoxedStrategy<ServerFrame> {
    let payload = prop_oneof![
        option::of(track())
            .prop_map(|track| { response::Payload::PublishTrack(PublishTrackResult { track }) }),
        option::of(role()).prop_map(|role| response::Payload::UpdateRole(RoleResult { role })),
    ];
    let response = (any::<u32>(), option::of(status()), option::of(payload)).prop_map(
        |(request_id, status, payload)| Response {
            request_id,
            status,
            payload,
        },
    );
    let body = prop_oneof![
        option::of(voice_state())
            .prop_map(|state| event::Body::VoiceStateUpdated(VoiceStateUpdated { state })),
        vec(role(), 0..3).prop_map(|roles| event::Body::RoleUpdated(RoleUpdated { roles })),
        (id(), enumeration(), any::<u64>()).prop_map(|(user_id, presence, last_seen_at_ms)| {
            event::Body::PresenceUpdated(PresenceUpdated {
                user_id,
                presence,
                last_seen_at_ms,
            })
        }),
        (
            mask(),
            vec(channel_permissions(), 0..4),
            option::of(role_limits())
        )
            .prop_map(|(server_permissions, channel_permissions, limits)| {
                event::Body::PermissionsUpdated(PermissionsUpdated {
                    server_permissions,
                    channel_permissions,
                    limits,
                })
            }),
    ];
    let event = (any::<u64>(), option::of(body)).prop_map(|(seq, body)| Event { seq, body });
    prop_oneof![
        response.prop_map(ServerFrame::from),
        event.prop_map(ServerFrame::from),
    ]
    .boxed()
}
