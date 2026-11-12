//! The hierarchy rules H1 to H10
//! (`docs/rust-rewrite/05-permissions-and-roles.md` section 5).
//!
//! Each function answers "may this actor do this to that target" and, on
//! refusal, says which rule refused ([`Denied::rule`]). The functions check
//! the hierarchy only. Whether the actor holds the permission the request
//! needs (`KickMembers`, `ManageRoles`, ...) is the job of [`crate::require`],
//! with one exception: H6 names `ManageRoles` itself, so the overwrite
//! checks include it.
//!
//! In every function:
//!
//! - the owner passes (H1), except where the rule is about the structure of
//!   the server rather than about rank (H8, H10, the validity of an
//!   overwrite);
//! - the owner is never a valid target for someone else, and never a valid
//!   target of a moderation action at all (H1);
//! - only position decides who outranks whom; holding `Administrator` makes
//!   no difference to rank (H9).

use core::cmp::Ordering;

use crate::denied::Denied;
use crate::escalation::{check_limit_change, check_permission_change};
use crate::limits::{RoleLimits, limit_grant_ceiling};
use crate::model::{
    ChannelView, MemberView, Overwrite, OverwriteTarget, Role, ServerView, Timestamp,
};
use crate::permissions::Permissions;
use crate::resolve::effective;

/// Passes when `target` is strictly below `actor`. Equal tops, and the actor
/// themself, are refused with H7; a higher target with `above`.
fn target_below_actor(
    actor: &MemberView<'_>,
    target: &MemberView<'_>,
    server: &ServerView<'_>,
    above: Denied,
) -> Result<(), Denied> {
    if actor.user_id == target.user_id {
        return Err(Denied::EqualRank);
    }
    match server.top_position(target).cmp(&server.top_position(actor)) {
        Ordering::Less => Ok(()),
        Ordering::Equal => Err(Denied::EqualRank),
        Ordering::Greater => Err(above),
    }
}

/// Passes when `position` is strictly below the actor's top position.
fn position_below_actor(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    position: u32,
    refusal: Denied,
) -> Result<(), Denied> {
    if position < server.top_position(actor) {
        Ok(())
    } else {
        Err(refusal)
    }
}

/// H4 for the mask and the limits of a role, measured against what the actor
/// holds server-wide.
fn check_role_grant(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    now: Timestamp,
    (old_permissions, new_permissions): (Permissions, Permissions),
    (old_limits, new_limits): (&RoleLimits, &RoleLimits),
) -> Result<(), Denied> {
    let held = effective(actor, server, None, now);
    check_permission_change(held, old_permissions, new_permissions)?;
    check_limit_change(&limit_grant_ceiling(actor, server), old_limits, new_limits)
}

/// H1, H5, H7: may `actor` kick, ban, time out, rename, server-mute,
/// server-deafen, move or disconnect `target`?
///
/// The target must be strictly below the actor. A member cannot moderate
/// themself this way (changing one's own nickname or leaving voice are
/// different requests). For a user who is not a member, pass a view without
/// roles.
pub fn check_moderate(
    actor: &MemberView<'_>,
    target: &MemberView<'_>,
    server: &ServerView<'_>,
) -> Result<(), Denied> {
    if server.is_owner(target.user_id) {
        return Err(Denied::OwnerIsTarget);
    }
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    target_below_actor(actor, target, server, Denied::TargetNotBelowActor)
}

/// H10: may `member` leave the server? Everyone but the owner may.
pub fn check_leave(member: &MemberView<'_>, server: &ServerView<'_>) -> Result<(), Denied> {
    if server.is_owner(member.user_id) {
        Err(Denied::OwnerCannotLeave)
    } else {
        Ok(())
    }
}

/// H2: is `role` strictly below the actor's top position?
///
/// This is the part of H2 that every operation on an existing role shares.
/// `@everyone` is at position 0, so any member with a role passes for it.
pub fn check_role_below(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    role: &Role,
) -> Result<(), Denied> {
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    position_below_actor(
        actor,
        server,
        server.position_of(role),
        Denied::RoleNotBelowActor,
    )
}

/// H2, H4, H8: may `actor` create a role at `position` with these
/// permissions and limits?
///
/// The position must be at least 1 (0 belongs to `@everyone`) and strictly
/// below the actor's top position. The actor must hold every permission bit
/// of the new role server-wide, and no limit may exceed the actor's own.
pub fn check_create_role(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    now: Timestamp,
    position: u32,
    permissions: Permissions,
    limits: &RoleLimits,
) -> Result<(), Denied> {
    if position == 0 {
        return Err(Denied::EveryoneRole);
    }
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    position_below_actor(actor, server, position, Denied::RoleNotBelowActor)?;
    check_role_grant(
        actor,
        server,
        now,
        (Permissions::EMPTY, permissions),
        (&RoleLimits::NONE, limits),
    )
}

/// H2, H4: may `actor` change `role` so that it ends up with
/// `new_permissions` and `new_limits`?
///
/// Pass the role's current values to check an edit that touches neither
/// (a rename, a new colour): then only H2 applies. `@everyone` can be edited
/// this way too (H8).
pub fn check_edit_role(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    now: Timestamp,
    role: &Role,
    new_permissions: Permissions,
    new_limits: &RoleLimits,
) -> Result<(), Denied> {
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    check_role_below(actor, server, role)?;
    check_role_grant(
        actor,
        server,
        now,
        (role.permissions, new_permissions),
        (&role.limits, new_limits),
    )
}

/// H2, H8: may `actor` delete `role`? `@everyone` cannot be deleted by
/// anyone.
pub fn check_delete_role(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    role: &Role,
) -> Result<(), Denied> {
    if server.is_everyone(role) {
        return Err(Denied::EveryoneRole);
    }
    check_role_below(actor, server, role)
}

/// H2, H8: may `actor` move `role` to `new_position`?
///
/// Both the role's current position and the new one must be strictly below
/// the actor's top position, so nobody can lift a role to their own level.
/// `@everyone` cannot be moved and no role can move to position 0.
pub fn check_move_role(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    role: &Role,
    new_position: u32,
) -> Result<(), Denied> {
    if server.is_everyone(role) || new_position == 0 {
        return Err(Denied::EveryoneRole);
    }
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    position_below_actor(
        actor,
        server,
        server.position_of(role),
        Denied::RoleNotBelowActor,
    )?;
    position_below_actor(actor, server, new_position, Denied::RoleNotBelowActor)
}

/// The part of H3 that assigning and revoking share, with H1, H7 and H8.
fn check_role_membership_change(
    actor: &MemberView<'_>,
    target: &MemberView<'_>,
    server: &ServerView<'_>,
    role: &Role,
) -> Result<(), Denied> {
    if server.is_everyone(role) {
        return Err(Denied::EveryoneRole);
    }
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    if server.is_owner(target.user_id) {
        return Err(Denied::OwnerIsTarget);
    }
    position_below_actor(
        actor,
        server,
        server.position_of(role),
        Denied::AssignedRoleNotBelowActor,
    )?;
    if actor.user_id == target.user_id {
        return Ok(());
    }
    target_below_actor(actor, target, server, Denied::AssignTargetNotBelowActor)
}

/// H1, H3, H4, H7, H8: may `actor` give `role` to `target`?
///
/// The role must be strictly below the actor's top position and, when the
/// target is someone else, so must the target. Assigning a role hands out
/// its permissions and limits, so H4 applies as well: the actor must hold
/// every permission bit of the role server-wide, and no limit of the role
/// may exceed the actor's own.
pub fn check_assign_role(
    actor: &MemberView<'_>,
    target: &MemberView<'_>,
    server: &ServerView<'_>,
    now: Timestamp,
    role: &Role,
) -> Result<(), Denied> {
    check_role_membership_change(actor, target, server, role)?;
    if server.is_owner(actor.user_id) {
        return Ok(());
    }
    check_role_grant(
        actor,
        server,
        now,
        (Permissions::EMPTY, role.permissions),
        (&RoleLimits::NONE, &role.limits),
    )
}

/// H1, H3, H7, H8: may `actor` take `role` away from `target`?
///
/// The same rank rules as [`check_assign_role`]. A member can drop a role of
/// their own only if it is strictly below their top position, so nobody
/// revokes their own top role.
pub fn check_revoke_role(
    actor: &MemberView<'_>,
    target: &MemberView<'_>,
    server: &ServerView<'_>,
    role: &Role,
) -> Result<(), Denied> {
    check_role_membership_change(actor, target, server, role)
}

/// The role or member an overwrite is for, resolved so that its rank can be
/// compared.
#[derive(Clone, Copy, Debug)]
pub enum OverwriteSubject<'a> {
    /// An overwrite for a role, `@everyone` included.
    Role(&'a Role),
    /// An overwrite for one member.
    Member(&'a MemberView<'a>),
}

impl OverwriteSubject<'_> {
    /// The target as it is stored in the overwrite.
    #[must_use]
    pub const fn target(&self) -> OverwriteTarget {
        match self {
            Self::Role(role) => OverwriteTarget::Role(role.id),
            Self::Member(member) => OverwriteTarget::Member(member.user_id),
        }
    }
}

/// H1, H4, H6, H7: may `actor` set the overwrite of `channel` for `subject`
/// to these masks? On success, returns the overwrite to store.
///
/// - The masks must not overlap, and the change must not add a server-scope
///   bit to either mask. This holds for the owner too.
/// - The actor needs `ManageRoles` in the channel.
/// - A role must be strictly below the actor's top position. A member must
///   be strictly below the actor; nobody edits their own member overwrite.
/// - Every bit that changes, in the allow mask or in the deny mask, must be
///   one the actor holds in the channel.
///
/// The current overwrite is read from `channel`, so the caller cannot pass a
/// stale one.
pub fn check_set_overwrite(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: &ChannelView<'_>,
    now: Timestamp,
    subject: OverwriteSubject<'_>,
    allow: Permissions,
    deny: Permissions,
) -> Result<Overwrite, Denied> {
    let target = subject.target();
    let overwrite = Overwrite::new(target, allow, deny)
        .map_err(|error| Denied::OverlappingOverwrite(error.overlap))?;

    let (old_allow, old_deny) = channel.masks_for(target);
    let changed = (old_allow ^ allow) | (old_deny ^ deny);
    // A server-scope bit that an old row already carries may stay or go;
    // it has no effect either way. A new one is refused.
    let added = (allow - old_allow) | (deny - old_deny);
    let server_scope = added & Permissions::SERVER_SCOPE;
    if !server_scope.is_empty() {
        return Err(Denied::ServerScopeInOverwrite(server_scope));
    }

    if server.is_owner(actor.user_id) {
        return Ok(overwrite);
    }
    if let OverwriteSubject::Member(member) = subject
        && server.is_owner(member.user_id)
    {
        return Err(Denied::OwnerIsTarget);
    }

    let held = effective(actor, server, Some(channel), now);
    if !held.contains(Permissions::MANAGE_ROLES) {
        return Err(Denied::ManageRolesRequired);
    }
    match subject {
        OverwriteSubject::Role(role) => position_below_actor(
            actor,
            server,
            server.position_of(role),
            Denied::OverwriteTargetNotBelowActor,
        )?,
        OverwriteSubject::Member(member) => {
            target_below_actor(actor, member, server, Denied::OverwriteTargetNotBelowActor)?;
        }
    }
    check_permission_change(held, Permissions::EMPTY, changed)?;
    Ok(overwrite)
}

/// H1, H4, H6, H7: may `actor` delete the overwrite of `channel` for
/// `subject`? Deleting is setting both masks to empty, with the same rules
/// as [`check_set_overwrite`].
pub fn check_delete_overwrite(
    actor: &MemberView<'_>,
    server: &ServerView<'_>,
    channel: &ChannelView<'_>,
    now: Timestamp,
    subject: OverwriteSubject<'_>,
) -> Result<(), Denied> {
    check_set_overwrite(
        actor,
        server,
        channel,
        now,
        subject,
        Permissions::EMPTY,
        Permissions::EMPTY,
    )
    .map(|_| ())
}
