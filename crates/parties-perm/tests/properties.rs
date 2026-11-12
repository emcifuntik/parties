//! Property tests for the invariants the design document states
//! (section 14), over generated servers.
//!
//! The model-based test of rule H4 has its own file, `model_h4.rs`.

mod common;

use std::cmp::Ordering;

use common::{Channel, Member, World, config, dense_permissions, limits, permissions, world};
use parties_perm::{
    ChannelId, Denied, LimitKind, Overwrite, OverwriteSubject, OverwriteTarget, Permissions, Role,
    RoleId, RoleLimits, check_assign_role, check_create_role, check_delete_overwrite,
    check_delete_role, check_edit_role, check_leave, check_moderate, check_move_role,
    check_revoke_role, check_role_below, check_set_overwrite, masks_for,
};
use proptest::prelude::*;
use proptest::sample::Index;

// ---------------------------------------------------------------------------
// A second implementation of section 4, written from the document alone.
// It works on plain integers and its own constants, so it shares nothing
// with the crate under test but the inputs.
// ---------------------------------------------------------------------------

mod reference {
    use super::{Channel, Member, OverwriteTarget, World};

    const ALL: u64 = u64::MAX >> 1;
    const VIEW_CHANNEL: u64 = 1 << 0;
    const SEND_MESSAGES: u64 = 1 << 16;
    const READ_MESSAGE_HISTORY: u64 = 1 << 17;
    const ATTACH_FILES: u64 = 1 << 18;
    const EMBED_LINKS: u64 = 1 << 19;
    const MENTION_EVERYONE: u64 = 1 << 21;
    const CONNECT: u64 = 1 << 32;
    const SPEAK: u64 = 1 << 33;
    const ADMINISTRATOR: u64 = 1 << 62;
    /// Bits 32 to 45.
    const VOICE_AND_VIDEO: u64 = 0x3FFF << 32;
    /// Bits 3, 4, 6 to 13 and 62.
    pub const SERVER_SCOPE: u64 = 0x4000_0000_0000_3FD8;

    fn overwrite_for(channel: &Channel, target: OverwriteTarget) -> Option<(u64, u64)> {
        channel
            .overwrites
            .iter()
            .find(|overwrite| overwrite.target() == target)
            .map(|overwrite| (overwrite.allow().bits(), overwrite.deny().bits()))
    }

    pub fn effective(world: &World, member: &Member, channel: Option<&Channel>) -> u64 {
        if member.id == world.owner {
            return ALL;
        }
        let roles = world.roles_of(member);

        // 1. Server-wide base: @everyone plus every role the member has.
        let mut perms = world.everyone.permissions.bits();
        for role in &roles {
            perms |= role.permissions.bits();
        }
        if perms & ADMINISTRATOR != 0 {
            return ALL;
        }

        // 2. Channel overwrites, least specific first.
        let base = perms;
        if let Some(channel) = channel {
            if let Some((allow, deny)) =
                overwrite_for(channel, OverwriteTarget::Role(world.everyone.id))
            {
                perms = (perms & !deny) | allow;
            }
            let (mut allow, mut deny) = (0, 0);
            for role in &roles {
                if let Some((role_allow, role_deny)) =
                    overwrite_for(channel, OverwriteTarget::Role(role.id))
                {
                    allow |= role_allow;
                    deny |= role_deny;
                }
            }
            if member.speak_grant == Some(channel.id) {
                allow |= SPEAK;
            }
            perms = (perms & !deny) | allow;
            if let Some((allow, deny)) = overwrite_for(channel, OverwriteTarget::Member(member.id))
            {
                perms = (perms & !deny) | allow;
            }
            perms = (perms & !SERVER_SCOPE) | (base & SERVER_SCOPE);
        }

        // 3. Implied restrictions: the rows of the table, in the order of
        // the table, repeated until nothing changes.
        let timed_out = member
            .timeout_until
            .is_some_and(|until| world.now.0 < until.0);
        let age = world.now.0.saturating_sub(member.joined_at.0);
        let restricted =
            age < world.policy.period_ms || (world.policy.require_role && member.roles.is_empty());
        loop {
            let before = perms;
            if channel.is_some() && perms & VIEW_CHANNEL == 0 {
                perms = 0;
            }
            if perms & CONNECT == 0 {
                perms &= !VOICE_AND_VIDEO;
            }
            if perms & SEND_MESSAGES == 0 {
                perms &= !(ATTACH_FILES | EMBED_LINKS | MENTION_EVERYONE);
            }
            if timed_out {
                perms &= VIEW_CHANNEL | READ_MESSAGE_HISTORY;
            }
            if restricted {
                perms &= !world.policy.restricted.bits();
            }
            if perms == before {
                return perms;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// `top(m)`, computed without the crate.
fn top(world: &World, member: &Member) -> u32 {
    world
        .roles_of(member)
        .iter()
        .map(|role| role.position)
        .max()
        .unwrap_or(0)
}

fn pick_member(world: &World, pick: Index) -> &Member {
    &world.members[pick.index(world.members.len())]
}

/// Any role of the world, `@everyone` included.
fn pick_role(world: &World, pick: Index) -> &Role {
    match pick.index(world.roles.len() + 1) {
        0 => &world.everyone,
        index => &world.roles[index - 1],
    }
}

/// A channel of the world, or an empty one when the world has none.
fn pick_channel(world: &World, pick: Index) -> Channel {
    if world.channels.is_empty() {
        Channel {
            id: ChannelId(999),
            overwrites: Vec::new(),
            caps: RoleLimits::UNLIMITED,
        }
    } else {
        world.channels[pick.index(world.channels.len())].clone()
    }
}

fn is_everyone(world: &World, role: &Role) -> bool {
    role.id == world.everyone.id
}

/// What rule H4 says about the mask and the limits of a role, in words.
fn expected_grant(
    held: Permissions,
    ceiling: &RoleLimits,
    (old_mask, new_mask): (Permissions, Permissions),
    (old_limits, new_limits): (&RoleLimits, &RoleLimits),
) -> Result<(), Denied> {
    let not_held = (old_mask ^ new_mask) - held;
    if !not_held.is_empty() {
        return Err(Denied::PermissionEscalation(not_held));
    }
    for kind in LimitKind::ALL {
        let value = new_limits.get(kind);
        if value != old_limits.get(kind) && value > ceiling.get(kind) {
            return Err(Denied::LimitEscalation(kind));
        }
    }
    Ok(())
}

/// What the rank rules say about one member acting on another, in words.
fn expected_rank(
    world: &World,
    actor: &Member,
    target: &Member,
    above: Denied,
) -> Result<(), Denied> {
    if actor.id == target.id {
        return Err(Denied::EqualRank);
    }
    match top(world, target).cmp(&top(world, actor)) {
        Ordering::Less => Ok(()),
        Ordering::Equal => Err(Denied::EqualRank),
        Ordering::Greater => Err(above),
    }
}

fn at_most(a: &RoleLimits, b: &RoleLimits) -> bool {
    LimitKind::ALL
        .into_iter()
        .all(|kind| a.get(kind) <= b.get(kind))
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(256, "tests/regressions/properties.txt"))]

    /// The algorithm in section 4, compared with a second implementation.
    #[test]
    fn effective_matches_the_documented_algorithm(world in world()) {
        for member in &world.members {
            prop_assert_eq!(
                world.effective(member, None).bits(),
                reference::effective(&world, member, None),
                "member {:?}, server-wide", member
            );
            for channel in &world.channels {
                prop_assert_eq!(
                    world.effective(member, Some(channel)).bits(),
                    reference::effective(&world, member, Some(channel)),
                    "member {:?}, channel {:?}", member, channel
                );
            }
        }
    }

    /// "Owner and administrator always resolve to all."
    #[test]
    fn owner_and_administrator_always_resolve_to_all(world in world(), grant in dense_permissions()) {
        // Make the second member an administrator through any of their
        // roles, or leave the world as it is when they have none.
        let mut world = world;
        if let Some(role) = world.members[1].roles.first().copied() {
            let role = world.roles.iter_mut().find(|candidate| candidate.id == role).unwrap();
            role.permissions = grant | Permissions::ADMINISTRATOR;
        }
        for member in &world.members {
            if !world.is_admin(member) {
                continue;
            }
            prop_assert_eq!(world.effective(member, None), Permissions::ALL);
            for channel in &world.channels {
                prop_assert_eq!(world.effective(member, Some(channel)), Permissions::ALL);
            }
        }
        prop_assert!(world.is_admin(&world.members[0]), "the first member is the owner");
    }

    /// "A member overwrite always overrides role overwrites."
    #[test]
    fn a_member_overwrite_always_overrides_role_overwrites(
        world in world(),
        member in any::<Index>(),
        channel in any::<Index>(),
        allow in dense_permissions(),
        deny in dense_permissions(),
    ) {
        let member = pick_member(&world, member);
        let mut channel = pick_channel(&world, channel);
        let target = OverwriteTarget::Member(member.id);
        let deny = deny - allow;
        channel.overwrites.retain(|overwrite| overwrite.target() != target);
        channel.overwrites.push(Overwrite::new(target, allow, deny).unwrap());

        // Before the implied restrictions, the member overwrite decides
        // every bit it names, whatever the roles and @everyone say.
        let raw = world.raw_in(member, &channel);
        let server_scope = Permissions::SERVER_SCOPE;
        prop_assert!(raw.contains(allow - server_scope), "allowed bits missing: {}", (allow - server_scope) - raw);
        prop_assert!(!raw.intersects(deny - server_scope), "denied bits present: {}", raw & (deny - server_scope));
        // Server-scope bits are exactly those of the base.
        prop_assert_eq!(raw & server_scope, world.base(member) & server_scope);

        // The implied restrictions only remove, so a denied bit stays
        // denied in the final result. Administrators are not subject to
        // overwrites at all.
        if !world.is_admin(member) {
            let effective = world.effective(member, Some(&channel));
            prop_assert!(!effective.intersects(deny - server_scope));
            prop_assert!(raw.contains(effective), "the restrictions added {}", effective - raw);
        }
    }

    /// "Adding a role never removes a server-wide bit." The same holds for
    /// limits.
    #[test]
    fn adding_a_role_never_removes_a_server_wide_bit(
        world in world(),
        member in any::<Index>(),
        role_pick in any::<Index>(),
        new_mask in permissions(),
        new_limits in limits(),
    ) {
        let index = member.index(world.members.len());
        let before = world.effective(&world.members[index], None);
        let limits_before = world.effective_limits(&world.members[index], None);
        let own_before = world.role_limits(&world.members[index]);

        // Either a role that already exists, or a new one on top.
        let mut after = world.clone();
        let existing = role_pick.index(after.roles.len() + 1);
        let added = if existing < after.roles.len() {
            after.roles[existing].id
        } else {
            let id = RoleId(900);
            after.roles.push(Role { id, position: 0, permissions: new_mask, limits: new_limits });
            after.renumber();
            id
        };
        if !after.members[index].roles.contains(&added) {
            after.members[index].roles.push(added);
        }

        let member = &after.members[index];
        let now = after.effective(member, None);
        prop_assert!(now.contains(before), "lost {}", before - now);
        prop_assert!(at_most(&limits_before, &after.effective_limits(member, None)));
        prop_assert!(at_most(&own_before, &after.role_limits(member)));
    }

    /// The result of resolution is closed under the implied restrictions,
    /// and it never holds bit 63.
    #[test]
    fn the_result_respects_every_implied_restriction(world in world()) {
        for member in &world.members {
            if world.is_admin(member) {
                continue;
            }
            let timed_out = member.timeout_until.is_some_and(|until| world.now < until);
            let young = world.now.0.saturating_sub(member.joined_at.0) < world.policy.period_ms;
            let restricted = young || (world.policy.require_role && member.roles.is_empty());
            let scopes = std::iter::once(None).chain(world.channels.iter().map(Some));
            for channel in scopes {
                let perms = world.effective(member, channel);
                prop_assert_eq!(perms.bits() >> 63, 0);
                if channel.is_some() && !perms.contains(Permissions::VIEW_CHANNEL) {
                    prop_assert_eq!(perms, Permissions::EMPTY, "a hidden channel grants something");
                }
                if !perms.contains(Permissions::CONNECT) {
                    prop_assert!(!perms.intersects(Permissions::VOICE_VIDEO));
                }
                if !perms.contains(Permissions::SEND_MESSAGES) {
                    prop_assert!(!perms.intersects(Permissions::REQUIRES_SEND_MESSAGES));
                }
                if timed_out {
                    prop_assert!(Permissions::KEPT_IN_TIMEOUT.contains(perms));
                }
                if restricted {
                    prop_assert!(!perms.intersects(world.policy.restricted));
                }
                // Server-scope bits never come from an overwrite.
                prop_assert!(world.base(member).contains(perms & Permissions::SERVER_SCOPE));
                prop_assert!(!perms.contains(Permissions::ADMINISTRATOR));
            }
        }
    }

    /// A channel without overwrites resolves like the server, except that a
    /// member who cannot view it holds nothing there.
    #[test]
    fn an_empty_channel_resolves_like_the_server(world in world()) {
        let channel = Channel { id: ChannelId(998), overwrites: Vec::new(), caps: RoleLimits::UNLIMITED };
        for member in &world.members {
            let server_wide = world.effective(member, None);
            let expected = if server_wide.contains(Permissions::VIEW_CHANNEL) {
                server_wide
            } else {
                Permissions::EMPTY
            };
            prop_assert_eq!(world.effective(member, Some(&channel)), expected);
        }
    }
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(256, "tests/regressions/properties.txt"))]

    /// "No request succeeds against a target at or above the actor's top
    /// position", with and without `Administrator` on the actor (H9).
    #[test]
    fn nothing_succeeds_against_a_target_at_or_above_the_actor(
        world in world(),
        actor in any::<Index>(),
        make_admin in any::<bool>(),
        mask in permissions(),
        new_limits in limits(),
        position in 0u32..8,
    ) {
        let mut world = world;
        let actor_index = 1 + actor.index(world.members.len() - 1);
        if make_admin {
            // H9: an administrator is bound by position like anyone else.
            let held = world.members[actor_index].roles.clone();
            for role in world.roles.iter_mut().filter(|role| held.contains(&role.id)) {
                role.permissions |= Permissions::ADMINISTRATOR;
            }
        }
        let server = world.server();
        let now = world.now;
        let actor = &world.members[actor_index];
        let actor_roles = world.roles_of(actor);
        let actor_view = actor.view(&actor_roles);
        let actor_top = top(&world, actor);

        // Members: the owner, the actor and everyone whose top is not below.
        for target in &world.members {
            if target.id != world.owner && target.id != actor.id && top(&world, target) < actor_top {
                continue;
            }
            let target_roles = world.roles_of(target);
            let target_view = target.view(&target_roles);
            prop_assert!(check_moderate(&actor_view, &target_view, &server).is_err());
            for channel in &world.channels {
                let subject = OverwriteSubject::Member(&target_view);
                let channel = channel.view();
                prop_assert!(check_set_overwrite(&actor_view, &server, &channel, now, subject, mask, Permissions::EMPTY).is_err());
                prop_assert!(check_delete_overwrite(&actor_view, &server, &channel, now, subject).is_err());
            }
            if target.id == actor.id {
                continue; // a member may manage their own lower roles (H3)
            }
            for role in &world.roles {
                prop_assert!(check_assign_role(&actor_view, &target_view, &server, now, role).is_err());
                prop_assert!(check_revoke_role(&actor_view, &target_view, &server, role).is_err());
            }
        }

        // Roles at or above the actor's top position.
        for role in world.roles.iter().filter(|role| role.position >= actor_top) {
            prop_assert!(check_role_below(&actor_view, &server, role).is_err());
            prop_assert!(check_edit_role(&actor_view, &server, now, role, mask, &new_limits).is_err());
            prop_assert!(check_edit_role(&actor_view, &server, now, role, role.permissions, &role.limits).is_err());
            prop_assert!(check_delete_role(&actor_view, &server, role).is_err());
            prop_assert!(check_move_role(&actor_view, &server, role, position).is_err());
            for target in &world.members {
                let target_roles = world.roles_of(target);
                let target_view = target.view(&target_roles);
                prop_assert!(check_assign_role(&actor_view, &target_view, &server, now, role).is_err());
                prop_assert!(check_revoke_role(&actor_view, &target_view, &server, role).is_err());
            }
            for channel in &world.channels {
                let subject = OverwriteSubject::Role(role);
                let channel = channel.view();
                prop_assert!(check_set_overwrite(&actor_view, &server, &channel, now, subject, mask, Permissions::EMPTY).is_err());
                prop_assert!(check_delete_overwrite(&actor_view, &server, &channel, now, subject).is_err());
            }
        }

        // Places at or above the actor's top position.
        if position >= actor_top {
            prop_assert!(check_create_role(&actor_view, &server, now, position, Permissions::EMPTY, &RoleLimits::NONE).is_err());
            for role in &world.roles {
                prop_assert!(check_move_role(&actor_view, &server, role, position).is_err());
            }
        }
    }

    /// "The owner passes every check", apart from the rules about the
    /// structure of the server (H8, H10, valid overwrites) and about the
    /// owner as a target (H1).
    #[test]
    fn the_owner_passes_every_check(
        world in world(),
        target in any::<Index>(),
        role_pick in any::<Index>(),
        channel in any::<Index>(),
        mask in dense_permissions(),
        other in dense_permissions(),
        new_limits in limits(),
        position in 1u32..100,
    ) {
        let server = world.server();
        let now = world.now;
        let owner = &world.members[0];
        let owner_roles = world.roles_of(owner);
        let owner_view = owner.view(&owner_roles);
        let target = &world.members[1 + target.index(world.members.len() - 1)];
        let target_roles = world.roles_of(target);
        let target_view = target.view(&target_roles);
        let role = pick_role(&world, role_pick);
        let channel = pick_channel(&world, channel);
        let channel = channel.view();

        prop_assert_eq!(check_moderate(&owner_view, &target_view, &server), Ok(()));
        prop_assert_eq!(check_role_below(&owner_view, &server, role), Ok(()));
        prop_assert_eq!(check_create_role(&owner_view, &server, now, position, mask, &new_limits), Ok(()));
        prop_assert_eq!(check_edit_role(&owner_view, &server, now, role, mask, &new_limits), Ok(()));
        if !is_everyone(&world, role) {
            prop_assert_eq!(check_delete_role(&owner_view, &server, role), Ok(()));
            prop_assert_eq!(check_move_role(&owner_view, &server, role, position), Ok(()));
            prop_assert_eq!(check_assign_role(&owner_view, &target_view, &server, now, role), Ok(()));
            prop_assert_eq!(check_revoke_role(&owner_view, &target_view, &server, role), Ok(()));
            prop_assert_eq!(check_assign_role(&owner_view, &owner_view, &server, now, role), Ok(()));
        }
        // Any valid overwrite: disjoint masks, no server-scope bit.
        let allow = mask - Permissions::SERVER_SCOPE;
        let deny = other - Permissions::SERVER_SCOPE - allow;
        for subject in [OverwriteSubject::Role(role), OverwriteSubject::Member(&target_view)] {
            let stored = check_set_overwrite(&owner_view, &server, &channel, now, subject, allow, deny);
            let expected = Overwrite::new(subject.target(), allow, deny).expect("disjoint by construction");
            prop_assert_eq!(stored, Ok(expected));
            prop_assert_eq!(check_delete_overwrite(&owner_view, &server, &channel, now, subject), Ok(()));
        }

        // And the owner is never a target, nor free to leave.
        prop_assert_eq!(check_moderate(&target_view, &owner_view, &server), Err(Denied::OwnerIsTarget));
        prop_assert_eq!(check_moderate(&owner_view, &owner_view, &server), Err(Denied::OwnerIsTarget));
        prop_assert_eq!(check_leave(&owner_view, &server), Err(Denied::OwnerCannotLeave));
        prop_assert_eq!(check_leave(&target_view, &server), Ok(()));
    }

    /// The checks on roles give exactly the answer the rules give in words,
    /// with the same reason.
    #[test]
    fn role_checks_agree_with_the_rules_in_words(
        world in world(),
        actor in any::<Index>(),
        target in any::<Index>(),
        role_pick in any::<Index>(),
        toggled in permissions(),
        drawn_limits in limits(),
        position in 0u32..8,
        held_only in any::<bool>(),
        keep_limits in any::<bool>(),
    ) {
        let server = world.server();
        let now = world.now;
        let actor = pick_member(&world, actor);
        let actor_roles = world.roles_of(actor);
        let actor_view = actor.view(&actor_roles);
        let target = pick_member(&world, target);
        let target_roles = world.roles_of(target);
        let target_view = target.view(&target_roles);
        let role = pick_role(&world, role_pick);

        let owner = actor.id == world.owner;
        let actor_top = top(&world, actor);
        let held = world.effective(actor, None);
        let ceiling = world.ceiling(actor);
        let role_position = if is_everyone(&world, role) { 0 } else { role.position };
        let below = |position: u32, refusal: Denied| if position < actor_top { Ok(()) } else { Err(refusal) };

        // Half of the time the request stays within what the actor holds,
        // so that the rank rules decide.
        let toggled = if held_only { toggled & held } else { toggled };
        let new_mask = role.permissions ^ toggled;
        let new_limits = match (keep_limits, held_only) {
            (true, _) => role.limits,
            (false, true) => drawn_limits.min(ceiling),
            (false, false) => drawn_limits,
        };

        // Edit: H2, then H4.
        let expected = if owner {
            Ok(())
        } else {
            below(role_position, Denied::RoleNotBelowActor).and_then(|()| {
                expected_grant(held, &ceiling, (role.permissions, new_mask), (&role.limits, &new_limits))
            })
        };
        prop_assert_eq!(check_edit_role(&actor_view, &server, now, role, new_mask, &new_limits), expected);

        // Create: H8, H2, then H4.
        let expected = if position == 0 {
            Err(Denied::EveryoneRole)
        } else if owner {
            Ok(())
        } else {
            below(position, Denied::RoleNotBelowActor).and_then(|()| {
                expected_grant(held, &ceiling, (Permissions::EMPTY, toggled), (&RoleLimits::NONE, &new_limits))
            })
        };
        prop_assert_eq!(check_create_role(&actor_view, &server, now, position, toggled, &new_limits), expected);

        // Delete and move: H8, then H2.
        let everyone = is_everyone(&world, role);
        let expected = if everyone {
            Err(Denied::EveryoneRole)
        } else if owner {
            Ok(())
        } else {
            below(role_position, Denied::RoleNotBelowActor)
        };
        prop_assert_eq!(check_delete_role(&actor_view, &server, role), expected);
        let expected = if everyone || position == 0 {
            Err(Denied::EveryoneRole)
        } else if owner {
            Ok(())
        } else {
            below(role_position, Denied::RoleNotBelowActor)
                .and_then(|()| below(position, Denied::RoleNotBelowActor))
        };
        prop_assert_eq!(check_move_role(&actor_view, &server, role, position), expected);

        // Revoke: H8, H1, H3, H7. Assign: the same, then H4.
        let rank = if everyone {
            Err(Denied::EveryoneRole)
        } else if owner {
            Ok(())
        } else if target.id == world.owner {
            Err(Denied::OwnerIsTarget)
        } else {
            below(role_position, Denied::AssignedRoleNotBelowActor).and_then(|()| {
                if actor.id == target.id {
                    Ok(())
                } else {
                    expected_rank(&world, actor, target, Denied::AssignTargetNotBelowActor)
                }
            })
        };
        prop_assert_eq!(check_revoke_role(&actor_view, &target_view, &server, role), rank);
        let expected = if owner {
            rank
        } else {
            rank.and_then(|()| {
                expected_grant(held, &ceiling, (Permissions::EMPTY, role.permissions), (&RoleLimits::NONE, &role.limits))
            })
        };
        prop_assert_eq!(check_assign_role(&actor_view, &target_view, &server, now, role), expected);

        // Moderate: H1, H5, H7.
        let expected = if target.id == world.owner {
            Err(Denied::OwnerIsTarget)
        } else if owner {
            Ok(())
        } else {
            expected_rank(&world, actor, target, Denied::TargetNotBelowActor)
        };
        prop_assert_eq!(check_moderate(&actor_view, &target_view, &server), expected);
    }

    /// The check on overwrites gives exactly the answer the rules give in
    /// words, with the same reason, and stores what was asked for.
    #[test]
    fn overwrite_checks_agree_with_the_rules_in_words(
        world in world(),
        actor in any::<Index>(),
        target in any::<Index>(),
        role_pick in any::<Index>(),
        channel in any::<Index>(),
        for_member in any::<bool>(),
        drawn_allow in permissions(),
        drawn_deny in permissions(),
        held_only in any::<bool>(),
    ) {
        let server = world.server();
        let now = world.now;
        let actor = pick_member(&world, actor);
        let actor_roles = world.roles_of(actor);
        let actor_view = actor.view(&actor_roles);
        let target = pick_member(&world, target);
        let target_roles = world.roles_of(target);
        let target_view = target.view(&target_roles);
        let role = pick_role(&world, role_pick);
        let channel = pick_channel(&world, channel);
        let subject = if for_member {
            OverwriteSubject::Member(&target_view)
        } else {
            OverwriteSubject::Role(role)
        };

        let owner = actor.id == world.owner;
        let held = world.effective(actor, Some(&channel));
        let (old_allow, old_deny) = masks_for(&channel.overwrites, subject.target());
        let server_scope = Permissions::from_bits(reference::SERVER_SCOPE).unwrap();

        // Half of the time the request is an edit of the stored overwrite
        // in bits the actor holds, so that the rank rules decide. The other
        // half is arbitrary and may even overlap.
        let (allow, deny) = if held_only {
            let allow = old_allow ^ ((drawn_allow & held) - server_scope);
            let deny = (old_deny ^ ((drawn_deny & held) - server_scope)) - allow;
            (allow, deny)
        } else {
            (drawn_allow, drawn_deny)
        };
        let changed = (old_allow ^ allow) | (old_deny ^ deny);
        let added = (allow - old_allow) | (deny - old_deny);

        let expected = if allow.intersects(deny) {
            Err(Denied::OverlappingOverwrite(allow & deny))
        } else if added.intersects(server_scope) {
            Err(Denied::ServerScopeInOverwrite(added & server_scope))
        } else if owner {
            Ok(())
        } else if for_member && target.id == world.owner {
            Err(Denied::OwnerIsTarget)
        } else if !held.contains(Permissions::MANAGE_ROLES) {
            Err(Denied::ManageRolesRequired)
        } else {
            let rank = if for_member {
                expected_rank(&world, actor, target, Denied::OverwriteTargetNotBelowActor)
            } else {
                let position = if is_everyone(&world, role) { 0 } else { role.position };
                if position < top(&world, actor) { Ok(()) } else { Err(Denied::OverwriteTargetNotBelowActor) }
            };
            rank.and_then(|()| {
                let not_held = changed - held;
                if not_held.is_empty() { Ok(()) } else { Err(Denied::PermissionEscalation(not_held)) }
            })
        };

        let actual = check_set_overwrite(&actor_view, &server, &channel.view(), now, subject, allow, deny);
        prop_assert_eq!(actual.map(|_| ()), expected);
        if let Ok(stored) = actual {
            prop_assert_eq!(stored.target(), subject.target());
            prop_assert_eq!((stored.allow(), stored.deny()), (allow, deny));
        }

        // Deleting is setting both masks to empty.
        let delete = check_delete_overwrite(&actor_view, &server, &channel.view(), now, subject);
        let as_set = check_set_overwrite(&actor_view, &server, &channel.view(), now, subject, Permissions::EMPTY, Permissions::EMPTY);
        prop_assert_eq!(delete, as_set.map(|_| ()));
    }

    /// Whatever a check allowed, the bits and limits it lets through are
    /// ones the actor holds: "nobody can grant what they do not hold".
    #[test]
    fn nobody_grants_what_they_do_not_hold(
        world in world(),
        actor in any::<Index>(),
        target in any::<Index>(),
        role_pick in any::<Index>(),
        channel in any::<Index>(),
        mask in permissions(),
        other in permissions(),
        new_limits in limits(),
        position in 1u32..8,
    ) {
        let server = world.server();
        let now = world.now;
        // Never the owner: the owner holds everything.
        let actor = &world.members[1 + actor.index(world.members.len() - 1)];
        let actor_roles = world.roles_of(actor);
        let actor_view = actor.view(&actor_roles);
        let target = pick_member(&world, target);
        let target_roles = world.roles_of(target);
        let target_view = target.view(&target_roles);
        let role = pick_role(&world, role_pick);
        let channel = pick_channel(&world, channel);

        let held = world.effective(actor, None);
        let ceiling = world.ceiling(actor);
        let granted_within = |old: &RoleLimits, new: &RoleLimits| {
            LimitKind::ALL.into_iter().all(|kind| new.get(kind) == old.get(kind) || new.get(kind) <= ceiling.get(kind))
        };

        if check_edit_role(&actor_view, &server, now, role, mask, &new_limits).is_ok() {
            prop_assert!(held.contains(mask - role.permissions), "edit added {}", (mask - role.permissions) - held);
            prop_assert!(granted_within(&role.limits, &new_limits));
        }
        if check_create_role(&actor_view, &server, now, position, mask, &new_limits).is_ok() {
            prop_assert!(held.contains(mask), "create granted {}", mask - held);
            prop_assert!(at_most(&new_limits, &ceiling));
        }
        if check_assign_role(&actor_view, &target_view, &server, now, role).is_ok() {
            prop_assert!(held.contains(role.permissions), "assign granted {}", role.permissions - held);
            prop_assert!(at_most(&role.limits, &ceiling));
        }

        let held_here = world.effective(actor, Some(&channel));
        let deny = other - mask;
        for subject in [OverwriteSubject::Role(role), OverwriteSubject::Member(&target_view)] {
            let (old_allow, old_deny) = masks_for(&channel.overwrites, subject.target());
            if check_set_overwrite(&actor_view, &server, &channel.view(), now, subject, mask, deny).is_ok() {
                // New allows are held, and so is every denial that was lifted.
                prop_assert!(held_here.contains(mask - old_allow));
                prop_assert!(held_here.contains(old_deny - deny));
                prop_assert!(held_here.contains(Permissions::MANAGE_ROLES));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Limits and the bit set
// ---------------------------------------------------------------------------

proptest! {
    #![proptest_config(config(256, "tests/regressions/properties.txt"))]

    /// The applied limits never exceed a cap, and for an ordinary member
    /// they are the member's own limits under the caps.
    #[test]
    fn applied_limits_stay_under_every_cap(world in world()) {
        for member in &world.members {
            let own = world.role_limits(member);
            for role in world.roles_of(member) {
                prop_assert!(at_most(&role.limits, &own));
            }
            prop_assert!(at_most(&world.everyone.limits, &own));

            let server_wide = world.effective_limits(member, None);
            prop_assert!(at_most(&server_wide, &world.caps));
            for kind in LimitKind::ALL {
                let expected = if world.is_admin(member) {
                    world.caps.get(kind)
                } else {
                    own.get(kind).min(world.caps.get(kind))
                };
                prop_assert_eq!(server_wide.get(kind), expected, "{:?}", kind);
            }
            for channel in &world.channels {
                let in_channel = world.effective_limits(member, Some(channel));
                for kind in LimitKind::ALL {
                    prop_assert_eq!(
                        in_channel.get(kind),
                        server_wide.get(kind).min(channel.caps.get(kind)),
                        "{:?}", kind
                    );
                }
            }
            // What may be granted is never below what is applied.
            prop_assert!(at_most(&server_wide, &world.ceiling(member)));
        }
    }

    /// The integer form round-trips and bit 63 stays clear under every
    /// operation.
    #[test]
    fn the_bit_set_is_closed_under_its_operations(a in any::<u64>(), b in any::<u64>()) {
        let x = Permissions::from_bits_truncate(a);
        let y = Permissions::from_bits_truncate(b);
        prop_assert_eq!(x.bits(), a & (u64::MAX >> 1));
        prop_assert_eq!(Permissions::from_bits(a).is_some(), a >> 63 == 0);
        prop_assert_eq!(Permissions::from_bits(x.bits()), Some(x));
        prop_assert_eq!(Permissions::from_i64(x.to_i64()), Some(x));
        prop_assert!(x.to_i64() >= 0);
        prop_assert_eq!(Permissions::from_i64(a.cast_signed()).is_some(), a >> 63 == 0);

        for result in [x | y, x & y, x ^ y, x - y, !x, !y] {
            prop_assert_eq!(result.bits() >> 63, 0);
        }
        prop_assert_eq!((x | y).bits(), x.bits() | y.bits());
        prop_assert_eq!((x & y).bits(), x.bits() & y.bits());
        prop_assert_eq!((x ^ y).bits(), x.bits() ^ y.bits());
        prop_assert_eq!((x - y).bits(), x.bits() & !y.bits());
        prop_assert_eq!(!!x, x);
        prop_assert_eq!(x.contains(y), x.bits() & y.bits() == y.bits());
        prop_assert_eq!(x.intersects(y), x.bits() & y.bits() != 0);
        prop_assert_eq!(x.count(), x.bits().count_ones());
        prop_assert_eq!(x.iter().collect::<Permissions>(), x);
        prop_assert!(x.iter().all(|bit| bit.count() == 1 && x.contains(bit)));
    }
}
