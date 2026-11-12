//! Fixture data: the "Glass" chat window of `docs/design/glass-2026-11`,
//! and a small in-memory stand-in for the client core ([`FixtureCore`]) that
//! answers commands so the window can be clicked through.
//!
//! Nothing here is product logic. When the client core exists it feeds the
//! same [`ShellState`] and this module stays for tests and screenshots.

mod core;

use std::path::{Path, PathBuf};

pub use self::core::FixtureCore;
use crate::{
    model::{
        Attachment, Avatar, ChannelId, Chat, ChatItem, Member, MemberGroup, MemberList, Message,
        MessageId, PartyHeader, PartyIcon, PartyId, PartyTab, Picture, Presence, Reaction,
        ReactionKind, ReplyReference, RoomActivity, RoomId, RoomMember, RoomRef, SelfState,
        ShellState, Streams, Switcher, SystemLine, SystemPart, TextChannel, UserId, View,
        VoiceRoom,
    },
    theme::LightId,
};

pub const NIGHT_SHIFT: PartyId = PartyId(1);
pub const CHANNEL_GENERAL: ChannelId = ChannelId(1);
pub const CHANNEL_CLIPS: ChannelId = ChannelId(2);
pub const CHANNEL_OFF_TOPIC: ChannelId = ChannelId(3);
pub const ROOM_GENERAL: RoomId = RoomId(1);
pub const ROOM_LATE_NIGHT: RoomId = RoomId(2);
pub const ROOM_QUIET_FOCUS: RoomId = RoomId(3);
pub const USER_TUXICK: UserId = UserId(1);
pub const MESSAGE_SARA: MessageId = MessageId(3);

/// Where the fixture finds pictures. The design's avatars are files that
/// Pencil generated into the repository's `images/` folder; they are not
/// part of the crate, so a missing file simply gives a monogram.
#[derive(Clone, Debug, Default)]
pub struct Pictures {
    dir: Option<PathBuf>,
}

impl Pictures {
    /// No pictures: every avatar is a monogram and the image attachment is
    /// an empty frame.
    pub fn none() -> Self {
        Self::default()
    }

    /// Pictures from the repository's `images/` folder.
    pub fn from_dir(dir: impl Into<PathBuf>) -> Self {
        Self {
            dir: Some(dir.into()),
        }
    }

    fn load(&self, file: &str) -> Option<Picture> {
        let path = self.dir.as_deref().map(|dir: &Path| dir.join(file))?;
        std::fs::read(path).ok().map(Picture::from_bytes)
    }

    /// The picture the Glass frames use for `name`, when Pencil generated
    /// one, else the smaller picture of the earlier Graphite frames.
    fn avatar(&self, name: &str) -> Avatar {
        let glass = match name {
            "tuxick" => Some("generated-1794350652970.png"),
            "IceTroll" => Some("generated-1794350703839.png"),
            "ivan" => Some("generated-1794350711511.png"),
            "Sara" => Some("generated-1794350717599.png"),
            "Maks" => Some("generated-1794350722883.png"),
            _ => None,
        };
        let picture = glass
            .and_then(|file| self.load(file))
            .or_else(|| self.load(&format!("graphite-{}-80.png", name.to_lowercase())));
        Avatar::monogram(name).with_picture(picture)
    }
}

/// The chat window of the Glass design in the Night light.
pub fn glass_shell(pictures: &Pictures) -> ShellState {
    let tuxick = pictures.avatar("tuxick");
    let ice_troll = pictures.avatar("IceTroll");
    let ivan = pictures.avatar("ivan");
    let sara = pictures.avatar("Sara");
    let maks = pictures.avatar("Maks");
    let maya = pictures.avatar("Maya");
    // The design draws these four without a picture of ours.
    let liam = Avatar::monogram("Liam");
    let nadia = Avatar::monogram("Nadia");
    let oleg = Avatar::monogram("Oleg");
    let kai = Avatar::monogram("Kai");

    let room_member = |id: u32, name: &str, avatar: &Avatar, activity| RoomMember {
        user: UserId(id),
        name: name.to_owned(),
        avatar: avatar.clone(),
        activity,
    };
    let member = |id: u32, name: &str, avatar: &Avatar, presence, status: &str| Member {
        user: UserId(id),
        name: name.to_owned(),
        avatar: avatar.clone(),
        presence,
        status: status.to_owned(),
        speaking: false,
    };

    ShellState {
        light: LightId::Night,
        switcher: Switcher {
            view: View::Party(NIGHT_SHIFT),
            parties: vec![
                party_tab(NIGHT_SHIFT, "Night Shift", PartyIcon::Moon, true),
                party_tab(PartyId(2), "Cinema Sunday", PartyIcon::Film, true),
                party_tab(PartyId(3), "Old Guild", PartyIcon::Shield, false),
                party_tab(PartyId(4), "Basement LAN", PartyIcon::Server, true),
            ],
            connected: Some(NIGHT_SHIFT),
        },
        party: PartyHeader {
            name: "Night Shift".to_owned(),
            icon: PartyIcon::Moon,
            locked: true,
            online: 12,
            capacity: 64,
        },
        voice_rooms: vec![
            VoiceRoom {
                id: ROOM_GENERAL,
                name: "General".to_owned(),
                members: vec![
                    room_member(1, "tuxick", &tuxick, RoomActivity::Listening),
                    room_member(2, "IceTroll", &ice_troll, RoomActivity::Speaking),
                    room_member(4, "ivan", &ivan, RoomActivity::SharingScreen),
                    room_member(3, "Sara", &sara, RoomActivity::Listening),
                    room_member(5, "Maks", &maks, RoomActivity::Muted),
                ],
            },
            VoiceRoom {
                id: ROOM_LATE_NIGHT,
                name: "Late night".to_owned(),
                members: vec![
                    room_member(6, "Maya", &maya, RoomActivity::Listening),
                    room_member(7, "Liam", &liam, RoomActivity::Deafened),
                ],
            },
            VoiceRoom {
                id: ROOM_QUIET_FOCUS,
                name: "Quiet focus".to_owned(),
                members: Vec::new(),
            },
        ],
        text_channels: vec![
            text_channel(CHANNEL_GENERAL, "general", false),
            text_channel(CHANNEL_CLIPS, "clips-and-links", false),
            text_channel(CHANNEL_OFF_TOPIC, "off-topic", true),
        ],
        selected_channel: CHANNEL_CLIPS,
        chat: clips_chat(pictures, &ice_troll, &ivan, &sara, &maks, &maya),
        draft: "Second round starts at 22:00, who is in?".to_owned(),
        members: MemberList {
            groups: vec![
                MemberGroup {
                    title: "Owner".to_owned(),
                    count: None,
                    members: vec![member(1, "tuxick", &tuxick, Presence::Online, "In General")],
                },
                MemberGroup {
                    title: "Moderators".to_owned(),
                    count: Some(2),
                    members: vec![
                        Member {
                            speaking: true,
                            ..member(
                                2,
                                "IceTroll",
                                &ice_troll,
                                Presence::Online,
                                "Speaking in General",
                            )
                        },
                        member(3, "Sara", &sara, Presence::Online, "In General"),
                    ],
                },
                MemberGroup {
                    title: "Online".to_owned(),
                    count: Some(4),
                    members: vec![
                        member(4, "ivan", &ivan, Presence::Online, "Sharing a screen"),
                        member(5, "Maks", &maks, Presence::Online, "In General"),
                        member(6, "Maya", &maya, Presence::Online, "In Late night"),
                        member(7, "Liam", &liam, Presence::Idle, "Idle, 12 min"),
                    ],
                },
                MemberGroup {
                    title: "Offline".to_owned(),
                    count: Some(24),
                    members: vec![
                        member(
                            8,
                            "Nadia",
                            &nadia,
                            Presence::Offline,
                            "Last seen 2 hours ago",
                        ),
                        member(9, "Oleg", &oleg, Presence::Offline, "Last seen yesterday"),
                        member(10, "Kai", &kai, Presence::Offline, "Last seen 3 days ago"),
                    ],
                },
            ],
            hidden: 21,
        },
        member_search: String::new(),
        members_panel_open: true,
        me: SelfState {
            user: USER_TUXICK,
            name: "tuxick".to_owned(),
            avatar: tuxick,
            presence: Presence::Online,
            room: Some(RoomRef {
                id: ROOM_GENERAL,
                name: "General".to_owned(),
            }),
            latency_ms: Some(24),
            muted: false,
            deafened: false,
            streams: Streams::default(),
        },
        open_menu: None,
    }
}

fn party_tab(id: PartyId, name: &str, icon: PartyIcon, reachable: bool) -> PartyTab {
    PartyTab {
        id,
        name: name.to_owned(),
        icon,
        reachable,
    }
}

fn text_channel(id: ChannelId, name: &str, unread: bool) -> TextChannel {
    TextChannel {
        id,
        name: name.to_owned(),
        unread,
    }
}

fn message(id: u32, author: &str, avatar: &Avatar, time: &str, text: &str) -> Message {
    Message {
        id: MessageId(id),
        author: author.to_owned(),
        avatar: avatar.clone(),
        time: time.to_owned(),
        text: text.to_owned(),
        reply_to: None,
        attachment: None,
        reactions: Vec::new(),
    }
}

fn clips_chat(
    pictures: &Pictures,
    ice_troll: &Avatar,
    ivan: &Avatar,
    sara: &Avatar,
    maks: &Avatar,
    maya: &Avatar,
) -> Chat {
    let scoreboard = "Final scoreboard from last night. The 1v3 at the end still makes no sense.";
    Chat {
        channel: CHANNEL_CLIPS,
        name: "clips-and-links".to_owned(),
        topic: "Clips, screenshots and links worth keeping.".to_owned(),
        items: vec![
            ChatItem::DayDivider("Today".to_owned()),
            ChatItem::from(Message {
                attachment: Some(Attachment::Image {
                    name: "scoreboard-final.png".to_owned(),
                    size: "1.4 MB".to_owned(),
                    picture: pictures.load("graphite-scoreboard.png"),
                }),
                ..message(1, "IceTroll", ice_troll, "18:41", scoreboard)
            }),
            ChatItem::from(Message {
                reply_to: Some(ReplyReference {
                    author: "IceTroll".to_owned(),
                    avatar: ice_troll.clone(),
                    excerpt: scoreboard.to_owned(),
                }),
                attachment: Some(Attachment::File {
                    name: "capture-1440p60-av1.json".to_owned(),
                    size: "4.2 KB".to_owned(),
                }),
                ..message(
                    2,
                    "ivan",
                    ivan,
                    "18:43",
                    "Recorded the whole thing. These are the capture settings I used for 1440p at 60 fps.",
                )
            }),
            ChatItem::from(Message {
                reactions: vec![
                    Reaction {
                        kind: ReactionKind::ThumbsUp,
                        count: 4,
                        mine: true,
                    },
                    Reaction {
                        kind: ReactionKind::Fire,
                        count: 2,
                        mine: false,
                    },
                ],
                ..message(
                    MESSAGE_SARA.0,
                    "Sara",
                    sara,
                    "18:44",
                    "Pinned both so they do not get buried. Keep them coming.",
                )
            }),
            ChatItem::System(SystemLine {
                parts: vec![
                    SystemPart::Name("Nadia".to_owned()),
                    SystemPart::Text("joined with a join code from".to_owned()),
                    SystemPart::Name("Sara".to_owned()),
                ],
                time: "19:02".to_owned(),
            }),
            ChatItem::NewMessages,
            ChatItem::from(message(
                4,
                "Maks",
                maks,
                "21:14",
                "Welcome, Nadia. Voice is in General whenever you want to drop in.",
            )),
            ChatItem::from(message(
                5,
                "Maya",
                maya,
                "21:16",
                "Adding the map callouts here too, so everything lives in one place.",
            )),
        ],
    }
}

/// The chats of the channels the design does not draw: short, so that
/// selecting a channel visibly changes the chat panel.
fn other_chat(channel: &TextChannel) -> Chat {
    let (topic, line) = match channel.id {
        CHANNEL_GENERAL => (
            "Everything that has no better place.",
            "Server restart went fine, everyone is back.",
        ),
        CHANNEL_OFF_TOPIC => (
            "Anything goes, within reason.",
            "Who left the kettle on again?",
        ),
        _ => ("", "Nothing here yet."),
    };
    Chat {
        channel: channel.id,
        name: channel.name.clone(),
        topic: topic.to_owned(),
        items: vec![
            ChatItem::DayDivider("Today".to_owned()),
            ChatItem::from(message(
                100 + channel.id.0,
                "Sara",
                &Avatar::monogram("Sara"),
                "17:20",
                line,
            )),
        ],
    }
}
