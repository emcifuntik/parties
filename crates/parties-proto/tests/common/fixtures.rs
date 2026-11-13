//! A realistic state snapshot: a small community with roles, categories, a
//! moderated voice channel, people in voice and a few published tracks.

use bytes::Bytes;
use parties_proto::ids::TimeSortableId;
use parties_proto::v2::{
    Channel, ChannelKind, ChannelPermissions, JoinPolicy, LimitCaps, Member, NewMemberPolicy,
    Overwrite, Presence, QueryVisibility, Ready, Role, RoleLimits, ServerSettings, Track,
    TrackKind, VideoCodec, VoiceState, overwrite,
};

/// 2026-11-13T12:00:00Z in milliseconds since the Unix epoch.
pub const NOW_UNIX_MS: u64 = 1_794_571_200_000;

const MINUTE_MS: u64 = 60_000;
const DAY_MS: u64 = 24 * 60 * MINUTE_MS;

/// The permission bits of `docs/rust-rewrite/05-permissions-and-roles.md`
/// section 3 that the fixture uses, by bit index.
pub mod bit {
    pub const VIEW_CHANNEL: u32 = 0;
    pub const VIEW_AUDIT_LOG: u32 = 4;
    pub const CREATE_JOIN_CODES: u32 = 5;
    pub const KICK_MEMBERS: u32 = 7;
    pub const TIMEOUT_MEMBERS: u32 = 9;
    pub const CHANGE_NICKNAME: u32 = 10;
    pub const MANAGE_NICKNAMES: u32 = 11;
    pub const APPROVE_MEMBERS: u32 = 13;
    pub const SEND_MESSAGES: u32 = 16;
    pub const READ_MESSAGE_HISTORY: u32 = 17;
    pub const ATTACH_FILES: u32 = 18;
    pub const EMBED_LINKS: u32 = 19;
    pub const ADD_REACTIONS: u32 = 20;
    pub const MANAGE_MESSAGES: u32 = 22;
    pub const PIN_MESSAGES: u32 = 23;
    pub const CONNECT: u32 = 32;
    pub const SPEAK: u32 = 33;
    pub const USE_VOICE_ACTIVITY: u32 = 34;
    pub const STREAM_SCREEN: u32 = 36;
    pub const STREAM_CAMERA: u32 = 37;
    pub const USE_AUX_AUDIO: u32 = 38;
    pub const MUTE_MEMBERS: u32 = 39;
    pub const DEAFEN_MEMBERS: u32 = 40;
    pub const MOVE_MEMBERS: u32 = 41;
    pub const DISCONNECT_MEMBERS: u32 = 42;
    pub const REQUEST_TO_SPEAK: u32 = 44;
    pub const ADMINISTRATOR: u32 = 62;
}

/// A permission mask with the given bits set.
pub fn mask(bits: &[u32]) -> u64 {
    bits.iter().fold(0, |mask, bit| mask | 1 << bit)
}

/// What `@everyone` may do on a new server (section 9 of the same document).
pub fn everyone_permissions() -> u64 {
    mask(&[
        bit::VIEW_CHANNEL,
        bit::CREATE_JOIN_CODES,
        bit::CHANGE_NICKNAME,
        bit::SEND_MESSAGES,
        bit::READ_MESSAGE_HISTORY,
        bit::ATTACH_FILES,
        bit::EMBED_LINKS,
        bit::ADD_REACTIONS,
        bit::CONNECT,
        bit::SPEAK,
        bit::USE_VOICE_ACTIVITY,
        bit::STREAM_SCREEN,
        bit::STREAM_CAMERA,
        bit::USE_AUX_AUDIO,
        bit::REQUEST_TO_SPEAK,
    ])
}

/// What the default Moderator role adds.
pub fn moderator_permissions() -> u64 {
    mask(&[
        bit::KICK_MEMBERS,
        bit::TIMEOUT_MEMBERS,
        bit::MANAGE_NICKNAMES,
        bit::MANAGE_MESSAGES,
        bit::PIN_MESSAGES,
        bit::MUTE_MEMBERS,
        bit::DEAFEN_MEMBERS,
        bit::MOVE_MEMBERS,
        bit::DISCONNECT_MEMBERS,
        bit::VIEW_AUDIT_LOG,
        bit::APPROVE_MEMBERS,
    ])
}

/// A time-sortable id assigned `days` and `sequence` after the server was
/// set up, half a year before [`NOW_UNIX_MS`].
pub fn id(days: u64, sequence: u32) -> u64 {
    let created = NOW_UNIX_MS - 180 * DAY_MS;
    TimeSortableId::from_unix_ms(created + days * DAY_MS, sequence)
        .expect("the fixture's instants are after the epoch")
        .get()
}

/// A stand-in for the SHA-512 of a picture: 64 bytes that depend on `seed`.
pub fn picture_hash(seed: u8) -> Bytes {
    (0..64u8)
        .map(|index| index.wrapping_mul(31).wrapping_add(seed))
        .collect()
}

pub const OWNER: u64 = 1;
pub const ME: u64 = 2;

fn user(number: u64) -> u64 {
    id(number, 0)
}

fn role_id(number: u32) -> u64 {
    id(0, 100 + number)
}

fn channel_id(number: u32) -> u64 {
    id(0, 200 + number)
}

const EVERYONE: u32 = 0;
const SPEAKER: u32 = 1;
const MODERATOR: u32 = 2;
const ADMIN: u32 = 3;

const VOICE_CATEGORY: u32 = 0;
const LOBBY: u32 = 1;
const STAGE: u32 = 2;
const AFK: u32 = 3;
const TEXT_CATEGORY: u32 = 4;
const GENERAL: u32 = 5;
const MODS: u32 = 6;

fn role_overwrite(role: u32, allow: u64, deny: u64) -> Overwrite {
    Overwrite {
        target: Some(overwrite::Target::RoleId(role_id(role))),
        allow,
        deny,
    }
}

fn roles() -> Vec<Role> {
    vec![
        Role {
            id: role_id(EVERYONE),
            revision: 3,
            name: "@everyone".to_owned(),
            color: 0,
            position: 0,
            permissions: everyone_permissions(),
            limits: Some(RoleLimits {
                max_video_bitrate: 8000,
                max_video_height: 1080,
                max_video_fps: 60,
                max_aux_audio_bitrate: 128,
                max_upload_size: 50,
                daily_upload_quota: 500,
                max_published_video_tracks: 2,
            }),
            hoisted: false,
            mentionable: false,
            requires_hardware_key: false,
        },
        Role {
            id: role_id(SPEAKER),
            revision: 1,
            name: "Speaker".to_owned(),
            color: 0x00C28F,
            position: 1,
            permissions: 0,
            limits: None,
            hoisted: false,
            mentionable: true,
            requires_hardware_key: false,
        },
        Role {
            id: role_id(MODERATOR),
            revision: 2,
            name: "Moderator".to_owned(),
            color: 0x4F8CFF,
            position: 2,
            permissions: moderator_permissions(),
            limits: Some(RoleLimits {
                max_video_bitrate: 12_000,
                max_video_height: 1440,
                ..RoleLimits::default()
            }),
            hoisted: true,
            mentionable: true,
            requires_hardware_key: false,
        },
        Role {
            id: role_id(ADMIN),
            revision: 1,
            name: "Admin".to_owned(),
            color: 0xFF5C5C,
            position: 3,
            permissions: mask(&[bit::ADMINISTRATOR]),
            limits: None,
            hoisted: true,
            mentionable: false,
            requires_hardware_key: true,
        },
    ]
}

fn channel(
    number: u32,
    kind: ChannelKind,
    parent: Option<u32>,
    position: u32,
    name: &str,
) -> Channel {
    Channel {
        id: channel_id(number),
        revision: 1,
        kind: kind.into(),
        parent_id: parent.map_or(0, channel_id),
        position,
        name: name.to_owned(),
        topic: String::new(),
        overwrites: Vec::new(),
        user_limit: 0,
        limit_caps: None,
        slow_mode_seconds: 0,
    }
}

fn channels() -> Vec<Channel> {
    let speak = mask(&[bit::SPEAK]);
    let view = mask(&[bit::VIEW_CHANNEL]);
    vec![
        channel(VOICE_CATEGORY, ChannelKind::Category, None, 0, "Voice"),
        Channel {
            user_limit: 12,
            ..channel(LOBBY, ChannelKind::Voice, Some(VOICE_CATEGORY), 0, "Lobby")
        },
        // A moderated channel: `@everyone` may listen, speakers may speak,
        // and one member is denied the right to raise a hand.
        Channel {
            revision: 7,
            overwrites: vec![
                role_overwrite(EVERYONE, 0, speak),
                role_overwrite(SPEAKER, speak, 0),
                Overwrite {
                    target: Some(overwrite::Target::UserId(user(6))),
                    allow: 0,
                    deny: mask(&[bit::REQUEST_TO_SPEAK]),
                },
            ],
            limit_caps: Some(LimitCaps {
                max_video_bitrate: Some(4000),
                max_video_height: Some(720),
                max_published_video_tracks: Some(1),
                ..LimitCaps::default()
            }),
            ..channel(STAGE, ChannelKind::Voice, Some(VOICE_CATEGORY), 1, "Stage")
        },
        channel(AFK, ChannelKind::Voice, Some(VOICE_CATEGORY), 2, "AFK"),
        channel(TEXT_CATEGORY, ChannelKind::Category, None, 1, "Text"),
        Channel {
            topic: "Anything goes. Be kind.".to_owned(),
            slow_mode_seconds: 5,
            ..channel(
                GENERAL,
                ChannelKind::Text,
                Some(TEXT_CATEGORY),
                0,
                "general",
            )
        },
        Channel {
            revision: 2,
            overwrites: vec![
                role_overwrite(EVERYONE, 0, view),
                role_overwrite(MODERATOR, view, 0),
            ],
            ..channel(MODS, ChannelKind::Text, Some(TEXT_CATEGORY), 1, "mods")
        },
    ]
}

fn member(number: u64, name: &str, roles: &[u32], presence: Presence) -> Member {
    Member {
        user_id: user(number),
        display_name: name.to_owned(),
        nickname: String::new(),
        role_ids: roles.iter().copied().map(role_id).collect(),
        picture_sha512: Bytes::new(),
        presence: presence.into(),
        joined_at_ms: NOW_UNIX_MS - (170 - number) * DAY_MS,
        timeout_until_ms: 0,
        server_muted: false,
        server_deafened: false,
        last_seen_at_ms: 0,
    }
}

fn me() -> Member {
    Member {
        nickname: "Mila (mod)".to_owned(),
        picture_sha512: picture_hash(2),
        ..member(ME, "Mila", &[SPEAKER, MODERATOR], Presence::Online)
    }
}

fn members() -> Vec<Member> {
    vec![
        Member {
            picture_sha512: picture_hash(1),
            ..member(OWNER, "Ève Čapek", &[ADMIN], Presence::Online)
        },
        me(),
        Member {
            picture_sha512: picture_hash(3),
            ..member(3, "Jonas", &[SPEAKER], Presence::Idle)
        },
        member(4, "久保田 玲", &[], Presence::DoNotDisturb),
        Member {
            last_seen_at_ms: NOW_UNIX_MS - 3 * DAY_MS,
            ..member(5, "Tariq", &[], Presence::Offline)
        },
        // Timed out for another ten minutes, and server-muted.
        Member {
            timeout_until_ms: NOW_UNIX_MS + 10 * MINUTE_MS,
            server_muted: true,
            ..member(6, "loud_guest", &[], Presence::Online)
        },
    ]
}

fn session(number: u64) -> u64 {
    0x5E55_0000 + number
}

fn voice_state(user_number: u64, channel: u32) -> VoiceState {
    VoiceState {
        session_id: session(user_number),
        user_id: user(user_number),
        channel_id: channel_id(channel),
        self_muted: false,
        self_deafened: false,
        speak_granted: false,
        decode_codecs: vec![
            VideoCodec::Av1.into(),
            VideoCodec::H265.into(),
            VideoCodec::H264.into(),
        ],
    }
}

fn voice_states() -> Vec<VoiceState> {
    vec![
        voice_state(OWNER, LOBBY),
        voice_state(ME, LOBBY),
        VoiceState {
            self_muted: true,
            self_deafened: true,
            decode_codecs: vec![VideoCodec::H264.into()],
            ..voice_state(3, LOBBY)
        },
        VoiceState {
            speak_granted: true,
            ..voice_state(4, STAGE)
        },
    ]
}

fn track(track_id: u32, kind: TrackKind, user_number: u64, channel: u32) -> Track {
    Track {
        id: track_id,
        kind: kind.into(),
        session_id: session(user_number),
        user_id: user(user_number),
        channel_id: channel_id(channel),
        linked_track_id: 0,
        codec: VideoCodec::Unspecified.into(),
        width: 0,
        height: 0,
    }
}

fn tracks() -> Vec<Track> {
    vec![
        track(1, TrackKind::Voice, OWNER, LOBBY),
        track(2, TrackKind::Voice, ME, LOBBY),
        track(3, TrackKind::Voice, 3, LOBBY),
        Track {
            codec: VideoCodec::Av1.into(),
            width: 2560,
            height: 1440,
            ..track(4, TrackKind::Screen, OWNER, LOBBY)
        },
        Track {
            linked_track_id: 4,
            ..track(5, TrackKind::ScreenAudio, OWNER, LOBBY)
        },
        Track {
            codec: VideoCodec::H264.into(),
            width: 1280,
            height: 720,
            ..track(6, TrackKind::Camera, ME, LOBBY)
        },
        track(7, TrackKind::Voice, 4, STAGE),
        track(8, TrackKind::AuxAudio, 4, STAGE),
    ]
}

fn settings() -> ServerSettings {
    ServerSettings {
        revision: 12,
        name: "Friday Night".to_owned(),
        description: "Games, films and whatever else comes up.".to_owned(),
        default_channel_id: channel_id(GENERAL),
        afk_channel_id: channel_id(AFK),
        afk_timeout_seconds: 600,
        join_policy: JoinPolicy::JoinCode.into(),
        join_password_set: false,
        min_identity_level: 8,
        new_members: Some(NewMemberPolicy {
            period_ms: 10 * MINUTE_MS,
            require_role: false,
            restricted: mask(&[
                bit::SPEAK,
                bit::SEND_MESSAGES,
                bit::STREAM_SCREEN,
                bit::STREAM_CAMERA,
                bit::USE_AUX_AUDIO,
            ]),
        }),
        max_members: 500,
        max_sessions: 0,
        max_sessions_per_member: 4,
        query_visibility: QueryVisibility::Basic.into(),
        limit_caps: Some(LimitCaps {
            max_video_bitrate: Some(12_000),
            max_video_height: Some(1440),
            max_upload_size: Some(100),
            ..LimitCaps::default()
        }),
        speaker_limit: 8,
        pictures_enabled: true,
        show_last_seen: true,
        max_join_code_lifetime_seconds: 30 * 24 * 60 * 60,
        max_unused_join_codes_per_member: 10,
    }
}

/// What the moderator may do server-wide.
fn server_permissions() -> u64 {
    everyone_permissions() | moderator_permissions()
}

fn channel_permissions() -> Vec<ChannelPermissions> {
    let server = server_permissions();
    // In every channel the moderator holds what they hold server-wide: in
    // the moderated channel the speaker role lifts the denial of `Speak` on
    // `@everyone`.
    [
        VOICE_CATEGORY,
        LOBBY,
        STAGE,
        AFK,
        TEXT_CATEGORY,
        GENERAL,
        MODS,
    ]
    .into_iter()
    .map(|number| ChannelPermissions {
        channel_id: channel_id(number),
        permissions: server,
    })
    .collect()
}

/// The snapshot a moderator of a small community receives.
pub fn ready() -> Ready {
    let members = members();
    Ready {
        more: false,
        seq: 48_211,
        settings: Some(settings()),
        owner_id: user(OWNER),
        everyone_role_id: role_id(EVERYONE),
        me: Some(me()),
        roles: roles(),
        channels: channels(),
        server_permissions: server_permissions(),
        channel_permissions: channel_permissions(),
        limits: Some(RoleLimits {
            max_video_bitrate: 12_000,
            max_video_height: 1440,
            max_video_fps: 60,
            max_aux_audio_bitrate: 128,
            max_upload_size: 50,
            daily_upload_quota: 500,
            max_published_video_tracks: 2,
        }),
        voice_states: voice_states(),
        tracks: tracks(),
        member_count: u32::try_from(members.len()).expect("a handful of members"),
        members,
        members_partial: false,
    }
}

/// A member directory of `count` members, the way a large server has one:
/// most offline, with pictures, nicknames and a few roles.
pub fn member_directory(count: u32) -> Vec<Member> {
    (0..count)
        .map(|index| {
            let number = u64::from(index) + 10;
            let presence = match index % 5 {
                0 => Presence::Online,
                1 => Presence::Idle,
                _ => Presence::Offline,
            };
            Member {
                user_id: id(number % 150, index),
                display_name: format!("member-{index:04}"),
                nickname: if index % 3 == 0 {
                    format!("Nickname of {index}")
                } else {
                    String::new()
                },
                role_ids: (0..index % 4).map(role_id).collect(),
                picture_sha512: picture_hash(index.to_le_bytes()[0]),
                presence: presence.into(),
                joined_at_ms: NOW_UNIX_MS - number * DAY_MS / 10,
                timeout_until_ms: 0,
                server_muted: false,
                server_deafened: false,
                last_seen_at_ms: if presence == Presence::Offline {
                    NOW_UNIX_MS - u64::from(index) * MINUTE_MS
                } else {
                    0
                },
            }
        })
        .collect()
}
