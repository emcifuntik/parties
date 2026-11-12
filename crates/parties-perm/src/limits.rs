//! Numeric role limits (`docs/rust-rewrite/05-permissions-and-roles.md`
//! section 6): "how much", where a permission bit can only answer "whether".

use crate::model::{ChannelView, MemberView, ServerView};
use crate::resolve::is_administrator;

/// Names one numeric limit.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LimitKind {
    /// Highest video bitrate of one published track, in kbit/s.
    MaxVideoBitrate,
    /// Highest video frame height, in pixels.
    MaxVideoHeight,
    /// Highest video frame rate, in frames per second.
    MaxVideoFps,
    /// Highest bitrate of auxiliary audio, in kbit/s.
    MaxAuxAudioBitrate,
    /// Largest single upload, in MiB.
    MaxUploadSize,
    /// Upload quota per day, in MiB.
    DailyUploadQuota,
    /// How many video tracks a member may publish at once.
    MaxPublishedVideoTracks,
}

impl LimitKind {
    /// Every limit, in the order the design document lists them.
    pub const ALL: [Self; 7] = [
        Self::MaxVideoBitrate,
        Self::MaxVideoHeight,
        Self::MaxVideoFps,
        Self::MaxAuxAudioBitrate,
        Self::MaxUploadSize,
        Self::DailyUploadQuota,
        Self::MaxPublishedVideoTracks,
    ];

    /// The stable name, used as the key in stored and transmitted limits.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::MaxVideoBitrate => "max_video_bitrate",
            Self::MaxVideoHeight => "max_video_height",
            Self::MaxVideoFps => "max_video_fps",
            Self::MaxAuxAudioBitrate => "max_aux_audio_bitrate",
            Self::MaxUploadSize => "max_upload_size",
            Self::DailyUploadQuota => "daily_upload_quota",
            Self::MaxPublishedVideoTracks => "max_published_video_tracks",
        }
    }

    /// The unit the value is expressed in.
    #[must_use]
    pub const fn unit(self) -> &'static str {
        match self {
            Self::MaxVideoBitrate | Self::MaxAuxAudioBitrate => "kbit/s",
            Self::MaxVideoHeight => "pixels",
            Self::MaxVideoFps => "frames/s",
            Self::MaxUploadSize | Self::DailyUploadQuota => "MiB",
            Self::MaxPublishedVideoTracks => "count",
        }
    }

    /// Looks a limit up by its stable name.
    #[must_use]
    pub fn from_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.name() == name)
    }
}

/// The numeric limits of one role, of one member, or a set of caps.
///
/// A member's limit is the maximum over their roles, so a role that does not
/// set a limit holds 0 for it and contributes nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct RoleLimits {
    /// See [`LimitKind::MaxVideoBitrate`].
    pub max_video_bitrate: u32,
    /// See [`LimitKind::MaxVideoHeight`].
    pub max_video_height: u32,
    /// See [`LimitKind::MaxVideoFps`].
    pub max_video_fps: u32,
    /// See [`LimitKind::MaxAuxAudioBitrate`].
    pub max_aux_audio_bitrate: u32,
    /// See [`LimitKind::MaxUploadSize`].
    pub max_upload_size: u32,
    /// See [`LimitKind::DailyUploadQuota`].
    pub daily_upload_quota: u32,
    /// See [`LimitKind::MaxPublishedVideoTracks`].
    pub max_published_video_tracks: u32,
}

const fn max_u32(a: u32, b: u32) -> u32 {
    if a > b { a } else { b }
}

const fn min_u32(a: u32, b: u32) -> u32 {
    if a < b { a } else { b }
}

impl RoleLimits {
    /// A role that sets no limit: all zero. It is the neutral element of
    /// [`RoleLimits::max`].
    pub const NONE: Self = Self {
        max_video_bitrate: 0,
        max_video_height: 0,
        max_video_fps: 0,
        max_aux_audio_bitrate: 0,
        max_upload_size: 0,
        daily_upload_quota: 0,
        max_published_video_tracks: 0,
    };

    /// No cap at all: every value is `u32::MAX`. It is the neutral element
    /// of [`RoleLimits::min`], so it stands for "this channel has no cap".
    pub const UNLIMITED: Self = Self {
        max_video_bitrate: u32::MAX,
        max_video_height: u32::MAX,
        max_video_fps: u32::MAX,
        max_aux_audio_bitrate: u32::MAX,
        max_upload_size: u32::MAX,
        daily_upload_quota: u32::MAX,
        max_published_video_tracks: u32::MAX,
    };

    /// The default limits of `@everyone` on a new server.
    pub const EVERYONE_DEFAULT: Self = Self {
        max_video_bitrate: 8000,
        max_video_height: 1080,
        max_video_fps: 60,
        max_aux_audio_bitrate: 128,
        max_upload_size: 50,
        daily_upload_quota: 500,
        max_published_video_tracks: 2,
    };

    /// Reads one limit.
    #[must_use]
    pub const fn get(&self, kind: LimitKind) -> u32 {
        match kind {
            LimitKind::MaxVideoBitrate => self.max_video_bitrate,
            LimitKind::MaxVideoHeight => self.max_video_height,
            LimitKind::MaxVideoFps => self.max_video_fps,
            LimitKind::MaxAuxAudioBitrate => self.max_aux_audio_bitrate,
            LimitKind::MaxUploadSize => self.max_upload_size,
            LimitKind::DailyUploadQuota => self.daily_upload_quota,
            LimitKind::MaxPublishedVideoTracks => self.max_published_video_tracks,
        }
    }

    /// Writes one limit.
    pub const fn set(&mut self, kind: LimitKind, value: u32) {
        match kind {
            LimitKind::MaxVideoBitrate => self.max_video_bitrate = value,
            LimitKind::MaxVideoHeight => self.max_video_height = value,
            LimitKind::MaxVideoFps => self.max_video_fps = value,
            LimitKind::MaxAuxAudioBitrate => self.max_aux_audio_bitrate = value,
            LimitKind::MaxUploadSize => self.max_upload_size = value,
            LimitKind::DailyUploadQuota => self.daily_upload_quota = value,
            LimitKind::MaxPublishedVideoTracks => self.max_published_video_tracks = value,
        }
    }

    /// Returns a copy with one limit replaced.
    #[must_use]
    pub const fn with(mut self, kind: LimitKind, value: u32) -> Self {
        self.set(kind, value);
        self
    }

    /// The larger value of each limit. This is how the roles of one member
    /// combine.
    #[must_use]
    pub const fn max(self, other: Self) -> Self {
        Self {
            max_video_bitrate: max_u32(self.max_video_bitrate, other.max_video_bitrate),
            max_video_height: max_u32(self.max_video_height, other.max_video_height),
            max_video_fps: max_u32(self.max_video_fps, other.max_video_fps),
            max_aux_audio_bitrate: max_u32(self.max_aux_audio_bitrate, other.max_aux_audio_bitrate),
            max_upload_size: max_u32(self.max_upload_size, other.max_upload_size),
            daily_upload_quota: max_u32(self.daily_upload_quota, other.daily_upload_quota),
            max_published_video_tracks: max_u32(
                self.max_published_video_tracks,
                other.max_published_video_tracks,
            ),
        }
    }

    /// The smaller value of each limit. This is how a cap applies.
    #[must_use]
    pub const fn min(self, other: Self) -> Self {
        Self {
            max_video_bitrate: min_u32(self.max_video_bitrate, other.max_video_bitrate),
            max_video_height: min_u32(self.max_video_height, other.max_video_height),
            max_video_fps: min_u32(self.max_video_fps, other.max_video_fps),
            max_aux_audio_bitrate: min_u32(self.max_aux_audio_bitrate, other.max_aux_audio_bitrate),
            max_upload_size: min_u32(self.max_upload_size, other.max_upload_size),
            daily_upload_quota: min_u32(self.daily_upload_quota, other.daily_upload_quota),
            max_published_video_tracks: min_u32(
                self.max_published_video_tracks,
                other.max_published_video_tracks,
            ),
        }
    }

    /// Whether every limit of `self` is at most the same limit of `other`.
    #[must_use]
    pub const fn fits_within(&self, other: &Self) -> bool {
        self.max_video_bitrate <= other.max_video_bitrate
            && self.max_video_height <= other.max_video_height
            && self.max_video_fps <= other.max_video_fps
            && self.max_aux_audio_bitrate <= other.max_aux_audio_bitrate
            && self.max_upload_size <= other.max_upload_size
            && self.daily_upload_quota <= other.daily_upload_quota
            && self.max_published_video_tracks <= other.max_published_video_tracks
    }
}

/// A member's own limits: the maximum over `@everyone` and every role the
/// member has. No cap is applied and administrators get no special
/// treatment here.
#[must_use]
pub fn role_limits(member: &MemberView<'_>, server: &ServerView<'_>) -> RoleLimits {
    member
        .roles
        .iter()
        .fold(server.everyone.limits, |limits, role| {
            limits.max(role.limits)
        })
}

/// The limits the server applies to a member: the minimum of the member's
/// own limits, the server-wide cap and, when a channel is given, that
/// channel's cap.
///
/// The owner and administrators get the server-wide cap instead of their own
/// limits. The channel's cap still applies to them.
#[must_use]
pub fn effective_limits(
    member: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: Option<&ChannelView<'_>>,
) -> RoleLimits {
    let capped = limit_grant_ceiling(member, server).min(server.limit_caps);
    match channel {
        Some(channel) => capped.min(channel.limit_caps),
        None => capped,
    }
}

/// The highest value of each limit that a member may put into a role or
/// hand out by assigning a role (rule H4).
///
/// For an ordinary member this is [`role_limits`], deliberately without the
/// server-wide cap: the cap bounds what is applied, not what a member holds,
/// so changing the cap later cannot turn an earlier grant into an
/// escalation. The owner and administrators are not bounded, because the
/// value applied to them is the server-wide cap itself, which already bounds
/// every role.
#[must_use]
pub fn limit_grant_ceiling(member: &MemberView<'_>, server: &ServerView<'_>) -> RoleLimits {
    if is_administrator(member, server) {
        RoleLimits::UNLIMITED
    } else {
        role_limits(member, server)
    }
}
