//! The permission catalogue: a 64-bit set with stable bit values, names,
//! groups and scopes (`docs/rust-rewrite/05-permissions-and-roles.md`
//! section 3).
//!
//! The bit values are part of the storage format and of the wire protocol.
//! They never change; `tests/catalogue.rs` pins every one of them.

use core::fmt;
use core::ops::{
    BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, Not, Sub, SubAssign,
};

/// The heading a permission is listed under in the catalogue and in the role
/// editor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PermissionGroup {
    /// Server structure, membership and moderation (bits 0 to 13).
    General,
    /// Text chat (bits 16 to 24).
    Text,
    /// Voice and video (bits 32 to 45).
    VoiceVideo,
    /// The administrator bypass (bit 62).
    Special,
}

impl PermissionGroup {
    /// Every group, in the order the catalogue lists them.
    pub const ALL: [Self; 4] = [Self::General, Self::Text, Self::VoiceVideo, Self::Special];

    /// The heading shown for this group.
    #[must_use]
    pub const fn title(self) -> &'static str {
        match self {
            Self::General => "General",
            Self::Text => "Text",
            Self::VoiceVideo => "Voice and video",
            Self::Special => "Special",
        }
    }

    /// The set of all defined permissions in this group.
    #[must_use]
    pub const fn mask(self) -> Permissions {
        Permissions(mask_of_group(self))
    }
}

/// Where a permission is evaluated.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PermissionScope {
    /// Scope **S**: evaluated server-wide only. Channel overwrites can
    /// neither grant nor remove the bit.
    Server,
    /// Scope **C**: can also be allowed or denied per channel.
    Channel,
}

/// One row of the permission catalogue.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PermissionInfo {
    /// The permission as a single-bit set.
    pub flag: Permissions,
    /// The bit index, 0 to 62.
    pub bit: u8,
    /// The stable name, for example `"ViewChannel"`.
    pub name: &'static str,
    /// The group the permission is listed under.
    pub group: PermissionGroup,
    /// Whether channel overwrites apply to the permission.
    pub scope: PermissionScope,
    /// One line that says what the permission grants.
    pub grants: &'static str,
}

/// A set of permissions: bits in a 64-bit mask.
///
/// Bit 63 is never set, so the value fits a signed 64-bit integer
/// ([`Permissions::to_i64`]). Reserved bits are kept as they are, so a role
/// edited by an older client keeps the bits a newer server understands.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Permissions(u64);

macro_rules! catalogue {
    ($( $bit:literal, $konst:ident, $name:literal, $group:ident, $scope:ident, $grants:literal; )+) => {
        impl Permissions {
            $(
                #[doc = concat!("`", $name, "`, bit ", stringify!($bit), ". ", $grants, ".")]
                pub const $konst: Self = Self(1 << $bit);
            )+
        }

        /// Every defined permission, in ascending bit order.
        pub const CATALOGUE: &[PermissionInfo] = &[
            $(
                PermissionInfo {
                    flag: Permissions::$konst,
                    bit: $bit,
                    name: $name,
                    group: PermissionGroup::$group,
                    scope: PermissionScope::$scope,
                    grants: $grants,
                },
            )+
        ];
    };
}

catalogue! {
    // General
    0, VIEW_CHANNEL, "ViewChannel", General, Channel, "See the channel, its members and activity";
    1, MANAGE_CHANNELS, "ManageChannels", General, Channel, "Create, edit, reorder and delete channels";
    2, MANAGE_ROLES, "ManageRoles", General, Channel, "Server-wide: create and edit roles below one's own. In a channel: edit its overwrites";
    3, MANAGE_SERVER, "ManageServer", General, Server, "Edit server settings";
    4, VIEW_AUDIT_LOG, "ViewAuditLog", General, Server, "Read the audit log";
    5, CREATE_JOIN_CODES, "CreateJoinCodes", General, Channel, "Create join codes";
    6, MANAGE_JOIN_CODES, "ManageJoinCodes", General, Server, "List and revoke others' join codes";
    7, KICK_MEMBERS, "KickMembers", General, Server, "Remove a member";
    8, BAN_MEMBERS, "BanMembers", General, Server, "Ban and unban";
    9, TIMEOUT_MEMBERS, "TimeoutMembers", General, Server, "Temporarily silence a member";
    10, CHANGE_NICKNAME, "ChangeNickname", General, Server, "Change one's own nickname";
    11, MANAGE_NICKNAMES, "ManageNicknames", General, Server, "Change others' nicknames";
    12, MANAGE_EXPRESSIONS, "ManageExpressions", General, Server, "Manage custom emoji and soundboard clips";
    13, APPROVE_MEMBERS, "ApproveMembers", General, Server, "Approve or reject join requests";
    // Text
    16, SEND_MESSAGES, "SendMessages", Text, Channel, "Post messages";
    17, READ_MESSAGE_HISTORY, "ReadMessageHistory", Text, Channel, "Read earlier messages, search, download attachments";
    18, ATTACH_FILES, "AttachFiles", Text, Channel, "Upload files";
    19, EMBED_LINKS, "EmbedLinks", Text, Channel, "Have links expanded into previews";
    20, ADD_REACTIONS, "AddReactions", Text, Channel, "Add new reactions";
    21, MENTION_EVERYONE, "MentionEveryone", Text, Channel, "Use mentions that notify everyone or a whole role";
    22, MANAGE_MESSAGES, "ManageMessages", Text, Channel, "Delete others' messages, remove reactions";
    23, PIN_MESSAGES, "PinMessages", Text, Channel, "Pin and unpin";
    24, BYPASS_SLOW_MODE, "BypassSlowMode", Text, Channel, "Ignore the channel's slow mode";
    // Voice and video
    32, CONNECT, "Connect", VoiceVideo, Channel, "Join the voice channel";
    33, SPEAK, "Speak", VoiceVideo, Channel, "Have one's voice forwarded";
    34, USE_VOICE_ACTIVITY, "UseVoiceActivity", VoiceVideo, Channel, "Transmit without push-to-talk";
    35, PRIORITY_SPEAKER, "PrioritySpeaker", VoiceVideo, Channel, "Others are attenuated while this member speaks";
    36, STREAM_SCREEN, "StreamScreen", VoiceVideo, Channel, "Publish a screen share and its audio";
    37, STREAM_CAMERA, "StreamCamera", VoiceVideo, Channel, "Publish a camera";
    38, USE_AUX_AUDIO, "UseAuxAudio", VoiceVideo, Channel, "Publish auxiliary audio: soundboard, music, application audio";
    39, MUTE_MEMBERS, "MuteMembers", VoiceVideo, Channel, "Server-mute others";
    40, DEAFEN_MEMBERS, "DeafenMembers", VoiceVideo, Channel, "Server-deafen others";
    41, MOVE_MEMBERS, "MoveMembers", VoiceVideo, Channel, "Move others between channels the actor can connect to";
    42, DISCONNECT_MEMBERS, "DisconnectMembers", VoiceVideo, Channel, "Remove others from voice";
    43, BYPASS_USER_LIMIT, "BypassUserLimit", VoiceVideo, Channel, "Join a full channel";
    44, REQUEST_TO_SPEAK, "RequestToSpeak", VoiceVideo, Channel, "Raise a hand in a moderated channel";
    45, SET_CHANNEL_STATUS, "SetChannelStatus", VoiceVideo, Channel, "Set the voice channel's status line";
    // Special
    62, ADMINISTRATOR, "Administrator", Special, Server, "Every permission, in every channel; overwrites do not apply";
}

/// Union of the catalogue entries whose scope is (`server == true`) or is not
/// (`server == false`) the server scope.
const fn mask_of_scope(server: bool) -> u64 {
    let mut mask = 0;
    let mut i = 0;
    while i < CATALOGUE.len() {
        let info = &CATALOGUE[i];
        if matches!(info.scope, PermissionScope::Server) == server {
            mask |= info.flag.0;
        }
        i += 1;
    }
    mask
}

/// Union of the catalogue entries listed under `group`.
const fn mask_of_group(group: PermissionGroup) -> u64 {
    let mut mask = 0;
    let mut i = 0;
    while i < CATALOGUE.len() {
        let info = &CATALOGUE[i];
        if info.group as u8 == group as u8 {
            mask |= info.flag.0;
        }
        i += 1;
    }
    mask
}

impl Permissions {
    /// The empty set.
    pub const EMPTY: Self = Self(0);

    /// Every bit from 0 to 62, reserved bits included. This is what the owner
    /// and administrators resolve to.
    pub const ALL: Self = Self(u64::MAX >> 1);

    /// Every permission the catalogue defines.
    pub const KNOWN: Self = Self(mask_of_scope(true) | mask_of_scope(false));

    /// The reserved bits (14 to 15, 25 to 31, 46 to 61). They are stored and
    /// passed through unchanged.
    pub const RESERVED: Self = Self(Self::ALL.0 & !Self::KNOWN.0);

    /// The permissions of scope **S**. Channel overwrites cannot grant or
    /// remove them.
    pub const SERVER_SCOPE: Self = Self(mask_of_scope(true));

    /// The defined permissions of scope **C**.
    pub const CHANNEL_SCOPE: Self = Self(mask_of_scope(false));

    /// The group "General".
    pub const GENERAL: Self = PermissionGroup::General.mask();

    /// The group "Text".
    pub const TEXT: Self = PermissionGroup::Text.mask();

    /// The group "Voice and video". A member without `Connect` in a channel
    /// holds none of these there.
    pub const VOICE_VIDEO: Self = PermissionGroup::VoiceVideo.mask();

    /// The permissions that need `SendMessages`: `AttachFiles`, `EmbedLinks`
    /// and `MentionEveryone`.
    pub const REQUIRES_SEND_MESSAGES: Self =
        Self(Self::ATTACH_FILES.0 | Self::EMBED_LINKS.0 | Self::MENTION_EVERYONE.0);

    /// What a timed-out member keeps: `ViewChannel` and `ReadMessageHistory`.
    pub const KEPT_IN_TIMEOUT: Self = Self(Self::VIEW_CHANNEL.0 | Self::READ_MESSAGE_HISTORY.0);

    /// Builds a set from its integer form. Returns `None` when bit 63 is
    /// set, because that bit is never used.
    #[must_use]
    pub const fn from_bits(bits: u64) -> Option<Self> {
        if bits & !Self::ALL.0 == 0 {
            Some(Self(bits))
        } else {
            None
        }
    }

    /// Builds a set from its integer form and drops bit 63.
    #[must_use]
    pub const fn from_bits_truncate(bits: u64) -> Self {
        Self(bits & Self::ALL.0)
    }

    /// Builds a set from a signed integer, the form SQLite stores. Returns
    /// `None` for a negative value.
    #[must_use]
    pub const fn from_i64(value: i64) -> Option<Self> {
        if value >= 0 {
            Some(Self(value.cast_unsigned()))
        } else {
            None
        }
    }

    /// The stable integer form of the set.
    #[must_use]
    pub const fn bits(self) -> u64 {
        self.0
    }

    /// The integer form as a signed value. It is never negative.
    #[must_use]
    pub const fn to_i64(self) -> i64 {
        self.0.cast_signed()
    }

    /// Whether no bit is set.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Whether every bit of `other` is set in `self`.
    #[must_use]
    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    /// Whether `self` and `other` share at least one bit.
    #[must_use]
    pub const fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }

    /// The bits set in `self` or in `other`.
    #[must_use]
    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    /// The bits set in both `self` and `other`.
    #[must_use]
    pub const fn intersection(self, other: Self) -> Self {
        Self(self.0 & other.0)
    }

    /// The bits set in `self` but not in `other`.
    #[must_use]
    pub const fn difference(self, other: Self) -> Self {
        Self(self.0 & !other.0)
    }

    /// The bits set in exactly one of `self` and `other`.
    #[must_use]
    pub const fn symmetric_difference(self, other: Self) -> Self {
        Self(self.0 ^ other.0)
    }

    /// Every bit from 0 to 62 that is not set in `self`.
    #[must_use]
    pub const fn complement(self) -> Self {
        Self(!self.0 & Self::ALL.0)
    }

    /// The number of bits set.
    #[must_use]
    pub const fn count(self) -> u32 {
        self.0.count_ones()
    }

    /// Iterates over the set bits as single-bit sets, lowest bit first.
    #[must_use]
    pub const fn iter(self) -> PermissionsIter {
        PermissionsIter(self.0)
    }

    /// The catalogue row of a single defined permission. Returns `None` for
    /// a set that is not exactly one defined bit.
    #[must_use]
    pub fn info(self) -> Option<&'static PermissionInfo> {
        CATALOGUE.iter().find(|info| info.flag == self)
    }

    /// The stable name of a single defined permission.
    #[must_use]
    pub fn name(self) -> Option<&'static str> {
        self.info().map(|info| info.name)
    }

    /// Looks a permission up by its stable name, for example `"Speak"`.
    #[must_use]
    pub fn from_name(name: &str) -> Option<Self> {
        CATALOGUE
            .iter()
            .find(|info| info.name == name)
            .map(|info| info.flag)
    }
}

/// Iterator over the bits of a [`Permissions`] set, lowest bit first.
#[derive(Clone, Debug)]
pub struct PermissionsIter(u64);

impl Iterator for PermissionsIter {
    type Item = Permissions;

    fn next(&mut self) -> Option<Permissions> {
        if self.0 == 0 {
            return None;
        }
        let lowest = self.0 & self.0.wrapping_neg();
        self.0 &= !lowest;
        Some(Permissions(lowest))
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let left = self.0.count_ones() as usize;
        (left, Some(left))
    }
}

impl ExactSizeIterator for PermissionsIter {}

impl IntoIterator for Permissions {
    type Item = Permissions;
    type IntoIter = PermissionsIter;

    fn into_iter(self) -> PermissionsIter {
        self.iter()
    }
}

impl FromIterator<Permissions> for Permissions {
    fn from_iter<I: IntoIterator<Item = Permissions>>(iter: I) -> Self {
        iter.into_iter().fold(Self::EMPTY, Self::union)
    }
}

impl BitOr for Permissions {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self {
        self.union(rhs)
    }
}

impl BitOrAssign for Permissions {
    fn bitor_assign(&mut self, rhs: Self) {
        *self = self.union(rhs);
    }
}

impl BitAnd for Permissions {
    type Output = Self;

    fn bitand(self, rhs: Self) -> Self {
        self.intersection(rhs)
    }
}

impl BitAndAssign for Permissions {
    fn bitand_assign(&mut self, rhs: Self) {
        *self = self.intersection(rhs);
    }
}

impl BitXor for Permissions {
    type Output = Self;

    fn bitxor(self, rhs: Self) -> Self {
        self.symmetric_difference(rhs)
    }
}

impl BitXorAssign for Permissions {
    fn bitxor_assign(&mut self, rhs: Self) {
        *self = self.symmetric_difference(rhs);
    }
}

/// Set difference: `a - b` is the bits of `a` that are not in `b`.
impl Sub for Permissions {
    type Output = Self;

    fn sub(self, rhs: Self) -> Self {
        self.difference(rhs)
    }
}

impl SubAssign for Permissions {
    fn sub_assign(&mut self, rhs: Self) {
        *self = self.difference(rhs);
    }
}

/// Complement within bits 0 to 62; bit 63 stays clear.
impl Not for Permissions {
    type Output = Self;

    fn not(self) -> Self {
        self.complement()
    }
}

/// Lists the names of the set bits, for example `ViewChannel | Speak`.
/// Reserved bits are shown as one hexadecimal number.
impl fmt::Display for Permissions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_empty() {
            return f.write_str("(none)");
        }
        if *self == Self::ALL {
            return f.write_str("ALL");
        }
        let mut first = true;
        for info in CATALOGUE {
            if self.contains(info.flag) {
                if !first {
                    f.write_str(" | ")?;
                }
                f.write_str(info.name)?;
                first = false;
            }
        }
        let reserved = self.0 & Self::RESERVED.0;
        if reserved != 0 {
            if !first {
                f.write_str(" | ")?;
            }
            write!(f, "{reserved:#x}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for Permissions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Permissions({self})")
    }
}
