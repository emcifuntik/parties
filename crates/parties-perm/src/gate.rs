//! The reusable parts of the request gate
//! (`docs/rust-rewrite/05-permissions-and-roles.md` section 7.1).
//!
//! The table that maps every request type to its requirement belongs to the
//! server, which owns the request types. What a requirement is made of does
//! not need them and lives here: where the permission is evaluated
//! ([`Scope`]), which hierarchy check applies ([`Hierarchy`],
//! [`check_hierarchy`]) and the permission test itself ([`require`]). The
//! "no escalation" checks are in [`crate::escalation`].

use crate::denied::Denied;
use crate::hierarchy::{check_moderate, check_role_below};
use crate::ids::{ChannelId, MessageId, RoleId, UserId};
use crate::model::{MemberView, Role, ServerView};
use crate::permissions::Permissions;

/// Where the permissions of a requirement are evaluated.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Scope {
    /// Server-wide: [`crate::effective`] without a channel.
    Server,
    /// In this channel.
    Channel(ChannelId),
    /// In the channel that holds this message. The caller looks the channel
    /// up; a message that does not exist has no channel and the request is
    /// refused.
    ChannelOf(MessageId),
}

/// Which hierarchy check a requirement names, by identifier.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Hierarchy {
    /// No hierarchy check.
    None,
    /// The request acts on this member (rule H5).
    TargetMember(UserId),
    /// The request acts on this role (rule H2).
    TargetRole(RoleId),
}

/// [`Hierarchy`] with its identifier resolved to the member or the role.
#[derive(Clone, Copy, Debug)]
pub enum HierarchyTarget<'a> {
    /// No hierarchy check.
    None,
    /// The request acts on this member (rule H5).
    Member(&'a MemberView<'a>),
    /// The request acts on this role (rule H2).
    Role(&'a Role),
}

/// The permission test of the gate: passes when `held` contains every bit
/// of `all_of`, and names the missing bits otherwise.
///
/// `held` is [`crate::effective`] of the actor in the scope of the request.
pub const fn require(held: Permissions, all_of: Permissions) -> Result<(), Denied> {
    let missing = all_of.difference(held);
    if missing.is_empty() {
        Ok(())
    } else {
        Err(Denied::MissingPermissions(missing))
    }
}

/// The hierarchy test of the gate: [`check_moderate`] for a member,
/// [`check_role_below`] for a role, nothing for [`HierarchyTarget::None`].
pub fn check_hierarchy(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    target: HierarchyTarget<'_>,
) -> Result<(), Denied> {
    match target {
        HierarchyTarget::None => Ok(()),
        HierarchyTarget::Member(member) => check_moderate(actor, member, server),
        HierarchyTarget::Role(role) => check_role_below(actor, server, role),
    }
}
