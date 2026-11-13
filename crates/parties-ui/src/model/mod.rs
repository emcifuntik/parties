//! What the main window shows, as plain data.
//!
//! Nothing here depends on the toolkit. The client core will produce these
//! values; until it exists, [`crate::fixtures`] does. The views read them
//! through [`crate::view_model::ShellModel`].

mod account;
mod chat;
mod home;
mod hotkeys;
mod picture;
mod settings;

pub use self::{
    account::{KeyEntry, KeyKind, Keys, PartyName, Profile},
    chat::{
        Attachment, Chat, ChatItem, Message, Reaction, ReactionKind, ReplyReference, SystemLine,
        SystemPart,
    },
    home::{Home, IdentityCard, KeyStorage, PartyCard, PartyLink, RoomPreview, UpdateNotice},
    hotkeys::{HotkeyAction, Hotkeys, Shortcut},
    picture::{Avatar, Picture},
    settings::{
        AppSettings, AppSwitch, CodecChoice, InputMode, SettingsGroup, SettingsPage, UpdateChannel,
        VideoSettings, VoiceSettings,
    },
};
use crate::theme::LightId;

macro_rules! id_type {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub struct $name(pub u32);
    };
}

id_type!(
    /// A party (the protocol calls it a server).
    PartyId
);
id_type!(
    /// A text channel of the party in view.
    ChannelId
);
id_type!(
    /// A voice room of the party in view.
    RoomId
);
id_type!(
    /// A member of the party in view.
    UserId
);
id_type!(
    /// A chat message.
    MessageId
);

/// Everything the main window shows.
#[derive(Clone, Debug, PartialEq)]
pub struct ShellState {
    pub light: LightId,
    /// Whether glass blurs what is behind it. Without the blur the panels
    /// are only translucent and the floating menus are opaque.
    pub backdrop_blur: bool,
    pub switcher: Switcher,
    /// A downloaded update, shown as a chip in the title bar.
    pub update: Option<UpdateNotice>,
    /// The Home view.
    pub home: Home,
    /// The page Settings is on.
    pub settings_page: SettingsPage,
    /// What the Profile page edits.
    pub profile: Profile,
    /// What the Keys page edits.
    pub keys: Keys,
    /// What the Voice and audio page edits.
    pub voice: VoiceSettings,
    /// What the Video and sharing page edits.
    pub video: VideoSettings,
    /// What the Hotkeys page edits.
    pub hotkeys: Hotkeys,
    /// What the App and notifications page edits.
    pub app: AppSettings,
    pub party: PartyHeader,
    pub voice_rooms: Vec<VoiceRoom>,
    pub text_channels: Vec<TextChannel>,
    pub selected_channel: ChannelId,
    pub chat: Chat,
    /// The text in the composer.
    pub draft: String,
    pub members: MemberList,
    /// The text in the member search field.
    pub member_search: String,
    pub members_panel_open: bool,
    pub me: SelfState,
    /// The menu open above the voice block, if any. One at most.
    pub open_menu: Option<VoiceBlockMenu>,
}

// --- Title bar ---

/// The view the window is on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum View {
    Home,
    Settings,
    Party(PartyId),
}

/// The switcher in the title bar: Home, Settings, then the parties.
#[derive(Clone, Debug, PartialEq)]
pub struct Switcher {
    pub view: View,
    pub parties: Vec<PartyTab>,
    /// The party the client is connected to. It stays lit on every view.
    pub connected: Option<PartyId>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PartyTab {
    pub id: PartyId,
    pub name: String,
    pub icon: PartyIcon,
    /// False while the party cannot be reached; its tile is dimmed.
    pub reachable: bool,
}

/// The built-in pictures a party can use as its tile.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PartyIcon {
    Moon,
    Film,
    Shield,
    Server,
}

// --- Left panel ---

#[derive(Clone, Debug, PartialEq)]
pub struct PartyHeader {
    pub name: String,
    pub icon: PartyIcon,
    /// The party only admits members with a join code.
    pub locked: bool,
    pub online: u32,
    pub capacity: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct VoiceRoom {
    pub id: RoomId,
    pub name: String,
    pub members: Vec<RoomMember>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RoomMember {
    pub user: UserId,
    pub name: String,
    pub avatar: Avatar,
    pub activity: RoomActivity,
}

/// What a member of a voice room is doing, as the channel list marks it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum RoomActivity {
    #[default]
    Listening,
    Speaking,
    SharingScreen,
    Muted,
    Deafened,
}

#[derive(Clone, Debug, PartialEq)]
pub struct TextChannel {
    pub id: ChannelId,
    pub name: String,
    pub unread: bool,
}

// --- Members ---

#[derive(Clone, Debug, PartialEq)]
pub struct MemberList {
    pub groups: Vec<MemberGroup>,
    /// Members of the last group that are not listed ("Show 21 more").
    pub hidden: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct MemberGroup {
    pub title: String,
    /// The number next to the title; `None` shows no number.
    pub count: Option<u32>,
    pub members: Vec<Member>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    pub user: UserId,
    pub name: String,
    pub avatar: Avatar,
    pub presence: Presence,
    /// One line of state: "In General", "Sharing a screen", "Last seen yesterday".
    pub status: String,
    pub speaking: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Presence {
    Online,
    Idle,
    DoNotDisturb,
    Offline,
}

impl Presence {
    /// The name the presence menu and the profile line show.
    pub const fn label(self) -> &'static str {
        match self {
            Self::Online => "Online",
            Self::Idle => "Idle",
            Self::DoNotDisturb => "Do not disturb",
            Self::Offline => "Offline",
        }
    }
}

// --- The user ---

#[derive(Clone, Debug, PartialEq)]
pub struct SelfState {
    pub user: UserId,
    pub name: String,
    pub avatar: Avatar,
    pub presence: Presence,
    /// The voice room the user is in.
    pub room: Option<RoomRef>,
    pub latency_ms: Option<u32>,
    pub muted: bool,
    pub deafened: bool,
    pub streams: Streams,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RoomRef {
    pub id: RoomId,
    pub name: String,
}

/// Which of the user's stream sources are live.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Streams {
    pub screen: bool,
    pub camera: bool,
    pub music: bool,
}

impl Streams {
    pub const fn is_live(self, source: StreamSource) -> bool {
        match source {
            StreamSource::Screen => self.screen,
            StreamSource::Camera => self.camera,
            StreamSource::Music => self.music,
        }
    }

    pub fn set(&mut self, source: StreamSource, live: bool) {
        match source {
            StreamSource::Screen => self.screen = live,
            StreamSource::Camera => self.camera = live,
            StreamSource::Music => self.music = live,
        }
    }

    pub fn live_count(self) -> usize {
        StreamSource::ALL
            .into_iter()
            .filter(|source| self.is_live(*source))
            .count()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamSource {
    Screen,
    Camera,
    Music,
}

impl StreamSource {
    pub const ALL: [StreamSource; 3] = [Self::Screen, Self::Camera, Self::Music];

    pub const fn label(self) -> &'static str {
        match self {
            Self::Screen => "Screen",
            Self::Camera => "Camera",
            Self::Music => "Music",
        }
    }
}

/// The menus that open upwards from the voice block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VoiceBlockMenu {
    Stream,
    Presence,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn streams_count_what_is_live() {
        let mut streams = Streams::default();
        assert_eq!(streams.live_count(), 0);
        streams.set(StreamSource::Screen, true);
        streams.set(StreamSource::Music, true);
        assert_eq!(streams.live_count(), 2);
        assert!(streams.is_live(StreamSource::Music));
        assert!(!streams.is_live(StreamSource::Camera));
    }
}
