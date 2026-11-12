//! Permission resolution (`docs/rust-rewrite/05-permissions-and-roles.md`
//! section 4): the effective permissions of a member, server-wide or in one
//! channel.
//!
//! [`effective`] is the function everything else calls. The three steps it
//! is made of are public too, so the client can show how a result came
//! about and so each step has its own tests.
//!
//! Nothing here allocates. The cost of one call is linear in the member's
//! roles plus, for a channel, the channel's overwrites times those roles.

use crate::model::{ChannelView, MemberView, OverwriteTarget, ServerView, Timestamp};
use crate::permissions::Permissions;

/// Step 1: the permissions of `@everyone` united with those of every role
/// the member has. Nothing is expanded or removed: an administrator's base
/// is just the bits of their roles.
#[must_use]
pub fn base_permissions(member: &MemberView<'_>, server: &ServerView<'_>) -> Permissions {
    member
        .roles
        .iter()
        .fold(server.everyone.permissions, |perms, role| {
            perms | role.permissions
        })
}

/// Whether the member bypasses resolution: the owner, or a holder of
/// `Administrator` through `@everyone` or any role.
#[must_use]
pub fn is_administrator(member: &MemberView<'_>, server: &ServerView<'_>) -> bool {
    server.is_owner(member.user_id)
        || base_permissions(member, server).contains(Permissions::ADMINISTRATOR)
}

/// Step 2: applies the overwrites of `channel` to the member's `base`,
/// least specific first.
///
/// 1. The overwrite for `@everyone`.
/// 2. The overwrites for the member's roles, united. Among roles, allow wins
///    over deny. A temporary speak grant for this channel counts as one more
///    role-level allow of `Speak`.
/// 3. The overwrite for the member, which has the last word.
///
/// Server-scope bits always come out as they are in `base`.
#[must_use]
pub fn apply_overwrites(
    base: Permissions,
    member: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: &ChannelView<'_>,
) -> Permissions {
    let everyone = server.everyone.id;
    let (mut everyone_allow, mut everyone_deny) = (Permissions::EMPTY, Permissions::EMPTY);
    let (mut roles_allow, mut roles_deny) = (Permissions::EMPTY, Permissions::EMPTY);
    let (mut member_allow, mut member_deny) = (Permissions::EMPTY, Permissions::EMPTY);

    for overwrite in channel.overwrites {
        match overwrite.target() {
            // Checked first, so listing `@everyone` among the member's roles
            // cannot lift its overwrite to the role level.
            OverwriteTarget::Role(role) if role == everyone => {
                everyone_allow |= overwrite.allow();
                everyone_deny |= overwrite.deny();
            }
            OverwriteTarget::Role(role) if member.has_role(role) => {
                roles_allow |= overwrite.allow();
                roles_deny |= overwrite.deny();
            }
            OverwriteTarget::Member(user) if user == member.user_id => {
                member_allow |= overwrite.allow();
                member_deny |= overwrite.deny();
            }
            OverwriteTarget::Role(_) | OverwriteTarget::Member(_) => {}
        }
    }
    if member.speak_grant == Some(channel.id) {
        roles_allow |= Permissions::SPEAK;
    }

    // One overwrite never allows and denies the same bit, so for the
    // `@everyone` and the member step the order of the two operations does
    // not matter. Deny is applied last so that duplicate rows for one
    // target, which storage forbids, fail closed.
    let mut perms = (base | everyone_allow) - everyone_deny;
    perms = (perms - roles_deny) | roles_allow;
    perms = (perms | member_allow) - member_deny;

    (perms - Permissions::SERVER_SCOPE) | (base & Permissions::SERVER_SCOPE)
}

/// Step 3: the implied restrictions, applied last.
///
/// | Condition | Effect |
/// |---|---|
/// | The member is timed out | Only `ViewChannel` and `ReadMessageHistory` remain |
/// | The new-member policy restricts the member | The policy's bits are removed |
/// | No `ViewChannel`, when `in_channel` | Nothing remains |
/// | No `Connect` | No permission of the group "Voice and video" |
/// | No `SendMessages` | No `AttachFiles`, `EmbedLinks`, `MentionEveryone` |
///
/// The two moderation rows run first, so the result is always closed under
/// the three dependency rows, even when the new-member policy removes
/// `ViewChannel`, `Connect` or `SendMessages`.
#[must_use]
pub fn apply_implications(
    perms: Permissions,
    member: &MemberView<'_>,
    server: &ServerView<'_>,
    in_channel: bool,
    now: Timestamp,
) -> Permissions {
    let mut perms = perms;
    if member.is_timed_out(now) {
        perms &= Permissions::KEPT_IN_TIMEOUT;
    }
    if server
        .new_members
        .applies_to(member, server.everyone.id, now)
    {
        perms -= server.new_members.restricted;
    }
    if in_channel && !perms.contains(Permissions::VIEW_CHANNEL) {
        return Permissions::EMPTY;
    }
    if !perms.contains(Permissions::CONNECT) {
        perms -= Permissions::VOICE_VIDEO;
    }
    if !perms.contains(Permissions::SEND_MESSAGES) {
        perms -= Permissions::REQUIRES_SEND_MESSAGES;
    }
    perms
}

/// The effective permissions of `member` at `now`: server-wide when
/// `channel` is `None`, in that channel otherwise.
///
/// The owner and administrators always get [`Permissions::ALL`]; overwrites,
/// timeouts and new-member restrictions do not apply to them.
#[must_use]
pub fn effective(
    member: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: Option<&ChannelView<'_>>,
    now: Timestamp,
) -> Permissions {
    if server.is_owner(member.user_id) {
        return Permissions::ALL;
    }
    let base = base_permissions(member, server);
    if base.contains(Permissions::ADMINISTRATOR) {
        return Permissions::ALL;
    }
    let perms = match channel {
        Some(channel) => apply_overwrites(base, member, server, channel),
        None => base,
    };
    apply_implications(perms, member, server, channel.is_some(), now)
}

/// Whether the member may be told about the channel at all: its name,
/// members, voice states, tracks and messages (section 7.3).
#[must_use]
pub fn can_view(
    member: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: &ChannelView<'_>,
    now: Timestamp,
) -> bool {
    effective(member, server, Some(channel), now).contains(Permissions::VIEW_CHANNEL)
}
