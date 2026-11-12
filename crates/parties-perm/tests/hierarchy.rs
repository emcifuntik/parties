//! Table-driven tests for the hierarchy rules H1 to H10 (section 5).
//!
//! The exhaustive tables walk every pair of positions around the actor's top
//! position, so "strictly below" cannot turn into "at or below" unnoticed.

mod common;

use common::{EVERYONE, NOW, OWNER, member_overwrite, role, role_overwrite};
use parties_perm::{
    ChannelId, ChannelView, Denied, LimitKind, MemberView, Overwrite, OverwriteSubject,
    OverwriteTarget, Permissions, Role, RoleLimits, Rule, ServerView, Timestamp, UserId,
    check_assign_role, check_create_role, check_delete_overwrite, check_delete_role,
    check_edit_role, check_leave, check_moderate, check_move_role, check_revoke_role,
    check_role_below, check_set_overwrite,
};

const VIEW: Permissions = Permissions::VIEW_CHANNEL;
const MANAGE_ROLES: Permissions = Permissions::MANAGE_ROLES;
const KICK: Permissions = Permissions::KICK_MEMBERS;
const BAN: Permissions = Permissions::BAN_MEMBERS;
const SEND: Permissions = Permissions::SEND_MESSAGES;
const ATTACH: Permissions = Permissions::ATTACH_FILES;
const PIN: Permissions = Permissions::PIN_MESSAGES;
const CONNECT: Permissions = Permissions::CONNECT;
const SPEAK: Permissions = Permissions::SPEAK;
const SCREEN: Permissions = Permissions::STREAM_SCREEN;
const ADMIN: Permissions = Permissions::ADMINISTRATOR;
const NONE: Permissions = Permissions::EMPTY;
const BIT_46: Permissions = Permissions::from_bits_truncate(1 << 46);

const ACTOR: UserId = UserId(2);
const TARGET: UserId = UserId(3);
const CHANNEL: ChannelId = ChannelId(500);

/// The highest position in the ladder.
const TOP: u32 = 5;

/// `@everyone` and one role at each position from 1 to [`TOP`]. The role at
/// position `p` has id `100 + p`.
struct Ladder {
    everyone: Role,
    rungs: Vec<Role>,
}

impl Ladder {
    /// Every role holds `rung_mask`; `@everyone` holds `everyone_mask`.
    fn new(everyone_mask: Permissions, rung_mask: Permissions) -> Self {
        Self {
            everyone: role(EVERYONE.0, 0, everyone_mask),
            rungs: (1..=TOP)
                .map(|position| role(100 + u64::from(position), position, rung_mask))
                .collect(),
        }
    }

    fn plain() -> Self {
        Self::new(NONE, NONE)
    }

    fn server(&self) -> ServerView<'_> {
        ServerView::new(OWNER, &self.everyone)
    }

    /// The role at `position`; position 0 is `@everyone`.
    fn at(&self, position: u32) -> &Role {
        match position {
            0 => &self.everyone,
            _ => &self.rungs[position as usize - 1],
        }
    }

    /// The roles of a member whose top position is `top`. With `padded`,
    /// every lower role is held as well, which must not change any answer.
    fn held(&self, top: u32, padded: bool) -> Vec<&Role> {
        match (top, padded) {
            (0, _) => Vec::new(),
            (_, false) => vec![self.at(top)],
            (_, true) => (1..=top).rev().map(|position| self.at(position)).collect(),
        }
    }
}

fn expect_below(lower: u32, upper: u32, refusal: Denied) -> Result<(), Denied> {
    if lower < upper { Ok(()) } else { Err(refusal) }
}

// ---------------------------------------------------------------------------
// H5, H7, H9: moderation
// ---------------------------------------------------------------------------

#[test]
fn moderation_needs_the_target_strictly_below() {
    for (actor_admin, target_admin) in [(false, false), (true, false), (false, true), (true, true)]
    {
        // H9: holding Administrator changes nothing about rank. Each side
        // gets its own ladder so that only one of them is an administrator.
        let actor_ladder = Ladder::new(NONE, if actor_admin { ADMIN } else { NONE });
        let target_ladder = Ladder::new(NONE, if target_admin { ADMIN } else { NONE });
        let server = actor_ladder.server();

        for actor_top in 0..=TOP {
            for target_top in 0..=TOP {
                for padded in [false, true] {
                    let actor_roles = actor_ladder.held(actor_top, padded);
                    let target_roles = target_ladder.held(target_top, padded);
                    let actor = MemberView::new(ACTOR, &actor_roles);
                    let target = MemberView::new(TARGET, &target_roles);

                    let expected = match target_top.cmp(&actor_top) {
                        std::cmp::Ordering::Less => Ok(()),
                        std::cmp::Ordering::Equal => Err(Denied::EqualRank),
                        std::cmp::Ordering::Greater => Err(Denied::TargetNotBelowActor),
                    };
                    assert_eq!(
                        check_moderate(&actor, &target, &server),
                        expected,
                        "actor top {actor_top} (admin {actor_admin}), target top {target_top} (admin {target_admin}), padded {padded}"
                    );
                }
            }
        }
    }
}

#[test]
fn nobody_moderates_themself() {
    let ladder = Ladder::plain();
    for top in 0..=TOP {
        let roles = ladder.held(top, false);
        let member = MemberView::new(ACTOR, &roles);
        assert_eq!(
            check_moderate(&member, &member, &ladder.server()),
            Err(Denied::EqualRank)
        );
    }

    // It is the user id that decides, even if the caller hands in a view of
    // the target that lists fewer roles than the view of the actor.
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let stale = MemberView::new(ACTOR, &[]);
    assert_eq!(
        check_moderate(&actor, &stale, &ladder.server()),
        Err(Denied::EqualRank)
    );
}

#[test]
fn a_user_who_is_not_a_member_has_no_rank() {
    // Banning by identity: the target has no roles at all.
    let ladder = Ladder::plain();
    let outsider = MemberView::new(TARGET, &[]);

    let one = ladder.held(1, false);
    assert_eq!(
        check_moderate(&MemberView::new(ACTOR, &one), &outsider, &ladder.server()),
        Ok(())
    );
    assert_eq!(
        check_moderate(&MemberView::new(ACTOR, &[]), &outsider, &ladder.server()),
        Err(Denied::EqualRank)
    );
}

// ---------------------------------------------------------------------------
// H1, H10: the owner
// ---------------------------------------------------------------------------

#[test]
fn the_owner_is_never_a_target() {
    let ladder = Ladder::new(NONE, ADMIN);
    let server = ladder.server();
    let top_roles = ladder.held(TOP, true);
    let admin = MemberView::new(ACTOR, &top_roles);
    let owner = MemberView::new(OWNER, &[]);
    let low = ladder.at(1);

    assert_eq!(
        check_moderate(&admin, &owner, &server),
        Err(Denied::OwnerIsTarget)
    );
    assert_eq!(
        check_assign_role(&admin, &owner, &server, NOW, low),
        Err(Denied::OwnerIsTarget)
    );
    assert_eq!(
        check_revoke_role(&admin, &owner, &server, low),
        Err(Denied::OwnerIsTarget)
    );

    let channel = ChannelView::new(CHANNEL, &[]);
    assert_eq!(
        check_set_overwrite(
            &admin,
            &server,
            &channel,
            NOW,
            OverwriteSubject::Member(&owner),
            SPEAK,
            NONE
        ),
        Err(Denied::OwnerIsTarget)
    );
    assert_eq!(
        check_delete_overwrite(
            &admin,
            &server,
            &channel,
            NOW,
            OverwriteSubject::Member(&owner)
        ),
        Err(Denied::OwnerIsTarget)
    );

    // Not even the owner moderates the owner: there is nobody to kick, ban
    // or time out them.
    assert_eq!(
        check_moderate(&owner, &owner, &server),
        Err(Denied::OwnerIsTarget)
    );
    assert_eq!(Denied::OwnerIsTarget.rule(), Some(Rule::H1));
}

#[test]
fn the_owner_passes_every_rank_check() {
    let ladder = Ladder::new(NONE, ADMIN);
    let server = ladder.server();
    let owner = MemberView::new(OWNER, &[]);
    let top_roles = ladder.held(TOP, true);
    let top_admin = MemberView::new(TARGET, &top_roles);
    let highest = ladder.at(TOP);
    let big = RoleLimits::UNLIMITED;
    let channel = ChannelView::new(CHANNEL, &[]);

    assert_eq!(check_moderate(&owner, &top_admin, &server), Ok(()));
    assert_eq!(check_role_below(&owner, &server, highest), Ok(()));
    assert_eq!(
        check_create_role(&owner, &server, NOW, TOP + 1, Permissions::ALL, &big),
        Ok(())
    );
    assert_eq!(
        check_create_role(&owner, &server, NOW, 1, Permissions::ALL, &big),
        Ok(())
    );
    assert_eq!(
        check_edit_role(&owner, &server, NOW, highest, Permissions::ALL, &big),
        Ok(())
    );
    assert_eq!(
        check_edit_role(
            &owner,
            &server,
            NOW,
            &ladder.everyone,
            Permissions::ALL,
            &big
        ),
        Ok(())
    );
    assert_eq!(check_delete_role(&owner, &server, highest), Ok(()));
    assert_eq!(check_move_role(&owner, &server, highest, 1), Ok(()));
    assert_eq!(
        check_move_role(&owner, &server, ladder.at(1), TOP + 3),
        Ok(())
    );
    assert_eq!(
        check_assign_role(&owner, &top_admin, &server, NOW, highest),
        Ok(())
    );
    assert_eq!(
        check_revoke_role(&owner, &top_admin, &server, highest),
        Ok(())
    );
    // The owner may hold roles too.
    assert_eq!(
        check_assign_role(&owner, &owner, &server, NOW, highest),
        Ok(())
    );
    assert_eq!(check_revoke_role(&owner, &owner, &server, highest), Ok(()));
    for subject in [
        OverwriteSubject::Role(highest),
        OverwriteSubject::Role(&ladder.everyone),
        OverwriteSubject::Member(&top_admin),
        OverwriteSubject::Member(&owner),
    ] {
        let stored = check_set_overwrite(&owner, &server, &channel, NOW, subject, SPEAK, VIEW);
        assert_eq!(
            stored.map(|overwrite| overwrite.target()),
            Ok(subject.target())
        );
        assert_eq!(
            check_delete_overwrite(&owner, &server, &channel, NOW, subject),
            Ok(())
        );
    }
}

#[test]
fn the_owner_cannot_leave() {
    let ladder = Ladder::new(NONE, ADMIN);
    let server = ladder.server();
    let top_roles = ladder.held(TOP, false);

    assert_eq!(
        check_leave(&MemberView::new(OWNER, &[]), &server),
        Err(Denied::OwnerCannotLeave)
    );
    assert_eq!(
        check_leave(&MemberView::new(OWNER, &top_roles), &server),
        Err(Denied::OwnerCannotLeave)
    );
    assert_eq!(check_leave(&MemberView::new(ACTOR, &[]), &server), Ok(()));
    assert_eq!(
        check_leave(&MemberView::new(ACTOR, &top_roles), &server),
        Ok(())
    );
    assert_eq!(Denied::OwnerCannotLeave.rule(), Some(Rule::H10));
}

// ---------------------------------------------------------------------------
// H2, H8, H9: roles
// ---------------------------------------------------------------------------

#[test]
fn a_role_is_managed_only_strictly_below_the_actor() {
    for admin in [false, true] {
        let ladder = Ladder::new(NONE, if admin { ADMIN } else { NONE });
        let server = ladder.server();

        for actor_top in 0..=TOP {
            for padded in [false, true] {
                let roles = ladder.held(actor_top, padded);
                let actor = MemberView::new(ACTOR, &roles);

                for position in 0..=TOP {
                    let target = ladder.at(position);
                    let context =
                        format!("actor top {actor_top}, role at {position}, admin {admin}");
                    let below = expect_below(position, actor_top, Denied::RoleNotBelowActor);

                    assert_eq!(
                        check_role_below(&actor, &server, target),
                        below,
                        "below: {context}"
                    );
                    // An edit that changes neither mask nor limits is H2 only.
                    assert_eq!(
                        check_edit_role(
                            &actor,
                            &server,
                            NOW,
                            target,
                            target.permissions,
                            &target.limits
                        ),
                        below,
                        "edit: {context}"
                    );
                    let delete = if position == 0 {
                        Err(Denied::EveryoneRole)
                    } else {
                        below
                    };
                    assert_eq!(
                        check_delete_role(&actor, &server, target),
                        delete,
                        "delete: {context}"
                    );

                    for new_position in 0..=TOP + 1 {
                        let moved = if position == 0 || new_position == 0 {
                            Err(Denied::EveryoneRole)
                        } else if position < actor_top && new_position < actor_top {
                            Ok(())
                        } else {
                            Err(Denied::RoleNotBelowActor)
                        };
                        assert_eq!(
                            check_move_role(&actor, &server, target, new_position),
                            moved,
                            "move to {new_position}: {context}"
                        );
                    }
                }

                for position in 0..=TOP + 1 {
                    let created = if position == 0 {
                        Err(Denied::EveryoneRole)
                    } else {
                        expect_below(position, actor_top, Denied::RoleNotBelowActor)
                    };
                    assert_eq!(
                        check_create_role(&actor, &server, NOW, position, NONE, &RoleLimits::NONE),
                        created,
                        "create at {position}: actor top {actor_top}, admin {admin}"
                    );
                }
            }
        }
    }
}

#[test]
fn everyone_cannot_be_deleted_moved_or_assigned() {
    let ladder = Ladder::new(VIEW, MANAGE_ROLES | ADMIN);
    let server = ladder.server();
    let everyone = &ladder.everyone;
    let top_roles = ladder.held(TOP, true);
    let low_roles = ladder.held(1, false);
    let owner = MemberView::new(OWNER, &[]);
    let admin = MemberView::new(ACTOR, &top_roles);
    let low = MemberView::new(TARGET, &low_roles);

    for (name, actor) in [("owner", &owner), ("administrator", &admin)] {
        assert_eq!(
            check_delete_role(actor, &server, everyone),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_move_role(actor, &server, everyone, 1),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_move_role(actor, &server, everyone, 0),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_move_role(actor, &server, ladder.at(1), 0),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_assign_role(actor, &low, &server, NOW, everyone),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_assign_role(actor, actor, &server, NOW, everyone),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_revoke_role(actor, &low, &server, everyone),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_revoke_role(actor, actor, &server, everyone),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        assert_eq!(
            check_create_role(actor, &server, NOW, 0, NONE, &RoleLimits::NONE),
            Err(Denied::EveryoneRole),
            "{name}"
        );
        // Its permissions can be edited (under H4).
        assert_eq!(
            check_edit_role(
                actor,
                &server,
                NOW,
                everyone,
                VIEW | SPEAK,
                &RoleLimits::NONE
            ),
            Ok(()),
            "{name}"
        );
    }
    assert_eq!(Denied::EveryoneRole.rule(), Some(Rule::H8));

    // The identity of @everyone is its id, whatever position the struct
    // carries.
    let misplaced = Role {
        position: 4,
        ..*everyone
    };
    assert_eq!(
        check_delete_role(&admin, &server, &misplaced),
        Err(Denied::EveryoneRole)
    );
    assert_eq!(check_role_below(&low, &server, &misplaced), Ok(()));
}

#[test]
fn editing_everyone_follows_h2_and_h4() {
    let ladder = Ladder::new(VIEW | SEND, MANAGE_ROLES);
    let server = ladder.server();
    let everyone = &ladder.everyone;
    let limits = RoleLimits::NONE;

    // A member with only @everyone is not above it, even if @everyone
    // itself hands out ManageRoles.
    let generous = role(EVERYONE.0, 0, VIEW | MANAGE_ROLES);
    let generous_server = ServerView::new(OWNER, &generous);
    let nobody = MemberView::new(ACTOR, &[]);
    assert_eq!(
        check_edit_role(&nobody, &generous_server, NOW, &generous, VIEW, &limits),
        Err(Denied::RoleNotBelowActor)
    );

    let roles = ladder.held(1, false);
    let manager = MemberView::new(ACTOR, &roles);
    let cases: &[(&str, Permissions, Result<(), Denied>)] = &[
        ("unchanged", VIEW | SEND, Ok(())),
        ("remove a bit the actor holds", VIEW, Ok(())),
        (
            "add a bit the actor holds",
            VIEW | SEND | MANAGE_ROLES,
            Ok(()),
        ),
        (
            "add a bit the actor lacks",
            VIEW | SEND | SPEAK,
            Err(Denied::PermissionEscalation(SPEAK)),
        ),
        (
            "add Administrator",
            VIEW | SEND | ADMIN,
            Err(Denied::PermissionEscalation(ADMIN)),
        ),
    ];
    for &(name, new_mask, expected) in cases {
        assert_eq!(
            check_edit_role(&manager, &server, NOW, everyone, new_mask, &limits),
            expected,
            "{name}"
        );
    }
}

// ---------------------------------------------------------------------------
// H3, H7, H9: assigning and revoking
// ---------------------------------------------------------------------------

#[test]
fn a_role_is_assigned_only_below_the_actor_and_to_someone_below() {
    for admin in [false, true] {
        let ladder = Ladder::new(NONE, if admin { ADMIN } else { NONE });
        // The target's roles carry no permissions, so that an administrator
        // actor never fails H4 and only rank is measured.
        let plain = Ladder::plain();
        let server = ladder.server();

        for actor_top in 0..=TOP {
            let actor_roles = ladder.held(actor_top, true);
            let actor = MemberView::new(ACTOR, &actor_roles);

            for position in 0..=TOP {
                let assigned = plain.at(position);

                for target_top in 0..=TOP {
                    let target_roles = plain.held(target_top, false);
                    let target = MemberView::new(TARGET, &target_roles);
                    let expected = if position == 0 {
                        Err(Denied::EveryoneRole)
                    } else if position >= actor_top {
                        Err(Denied::AssignedRoleNotBelowActor)
                    } else {
                        match target_top.cmp(&actor_top) {
                            std::cmp::Ordering::Less => Ok(()),
                            std::cmp::Ordering::Equal => Err(Denied::EqualRank),
                            std::cmp::Ordering::Greater => Err(Denied::AssignTargetNotBelowActor),
                        }
                    };
                    let context = format!(
                        "actor top {actor_top}, role at {position}, target top {target_top}, admin {admin}"
                    );
                    assert_eq!(
                        check_assign_role(&actor, &target, &server, NOW, assigned),
                        expected,
                        "assign: {context}"
                    );
                    assert_eq!(
                        check_revoke_role(&actor, &target, &server, assigned),
                        expected,
                        "revoke: {context}"
                    );
                }

                // Self-service: only the role's position matters.
                let own = if position == 0 {
                    Err(Denied::EveryoneRole)
                } else {
                    expect_below(position, actor_top, Denied::AssignedRoleNotBelowActor)
                };
                let context = format!("actor top {actor_top}, role at {position}, admin {admin}");
                assert_eq!(
                    check_assign_role(&actor, &actor, &server, NOW, assigned),
                    own,
                    "assign to self: {context}"
                );
                assert_eq!(
                    check_revoke_role(&actor, &actor, &server, assigned),
                    own,
                    "revoke from self: {context}"
                );
            }
        }
    }
}

#[test]
fn nobody_revokes_their_own_top_role() {
    let ladder = Ladder::new(NONE, ADMIN);
    let roles = ladder.held(3, true);
    let member = MemberView::new(ACTOR, &roles);
    let server = ladder.server();

    assert_eq!(
        check_revoke_role(&member, &member, &server, ladder.at(3)),
        Err(Denied::AssignedRoleNotBelowActor)
    );
    assert_eq!(
        check_revoke_role(&member, &member, &server, ladder.at(2)),
        Ok(())
    );
}

// ---------------------------------------------------------------------------
// H4: no escalation through roles
// ---------------------------------------------------------------------------

/// The actor of the H4 tables: `@everyone` gives `VIEW | SEND`, the actor's
/// top role at position 3 adds `MANAGE_ROLES | KICK | ATTACH`.
fn h4_ladder() -> Ladder {
    let mut ladder = Ladder::new(VIEW | SEND, NONE);
    ladder.rungs[2].permissions = MANAGE_ROLES | KICK | ATTACH;
    ladder
}

#[test]
fn a_role_mask_changes_only_in_bits_the_actor_holds() {
    let mut ladder = h4_ladder();
    // The role under edit already holds a bit the actor lacks (SPEAK) and
    // one the actor holds (SEND).
    ladder.rungs[0].permissions = SPEAK | SEND;
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let edited = ladder.at(1);
    let limits = RoleLimits::NONE;
    let escalation = |bits| Err(Denied::PermissionEscalation(bits));

    let cases: &[(&str, Permissions, Result<(), Denied>)] = &[
        ("unchanged", SPEAK | SEND, Ok(())),
        ("add a held bit", SPEAK | SEND | KICK, Ok(())),
        (
            "add every held bit",
            SPEAK | VIEW | SEND | MANAGE_ROLES | KICK | ATTACH,
            Ok(()),
        ),
        ("remove a held bit", SPEAK, Ok(())),
        (
            "add a bit the actor lacks",
            SPEAK | SEND | BAN,
            escalation(BAN),
        ),
        ("add Administrator", SPEAK | SEND | ADMIN, escalation(ADMIN)),
        (
            "add a reserved bit the actor lacks",
            SPEAK | SEND | BIT_46,
            escalation(BIT_46),
        ),
        ("remove a bit the actor lacks", SEND, escalation(SPEAK)),
        ("clear the mask", NONE, escalation(SPEAK)),
        (
            "swap a held bit for one the actor lacks",
            SPEAK | BAN,
            escalation(BAN),
        ),
        (
            "several bits the actor lacks",
            SPEAK | SEND | BAN | PIN | KICK,
            escalation(BAN | PIN),
        ),
        (
            "add one and remove one the actor lacks",
            SEND | BAN,
            escalation(SPEAK | BAN),
        ),
        (
            "everything",
            Permissions::ALL,
            escalation(Permissions::ALL - (VIEW | SEND | MANAGE_ROLES | KICK | ATTACH | SPEAK)),
        ),
    ];
    for &(name, new_mask, expected) in cases {
        assert_eq!(
            check_edit_role(&actor, &server, NOW, edited, new_mask, &limits),
            expected,
            "{name}"
        );
        assert_eq!(
            expected.err().and_then(|denied| denied.rule()),
            expected.err().map(|_| Rule::H4),
            "{name}"
        );
    }
}

#[test]
fn what_the_actor_holds_is_their_effective_server_wide_set() {
    let ladder = h4_ladder();
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let edited = ladder.at(1);
    let limits = RoleLimits::NONE;

    // AttachFiles is held, but a timed-out member holds only ViewChannel
    // and ReadMessageHistory.
    let actor = MemberView::new(ACTOR, &roles);
    assert_eq!(
        check_edit_role(&actor, &server, NOW, edited, ATTACH, &limits),
        Ok(())
    );
    let timed_out = MemberView {
        timeout_until: Some(Timestamp(NOW.0 + 1)),
        ..actor
    };
    assert_eq!(
        check_edit_role(&timed_out, &server, NOW, edited, ATTACH | VIEW, &limits),
        Err(Denied::PermissionEscalation(ATTACH))
    );
    assert_eq!(
        check_edit_role(&timed_out, &server, NOW, edited, VIEW, &limits),
        Ok(())
    );
    // The same request passes once the timeout has ended.
    let later = Timestamp(NOW.0 + 1);
    assert_eq!(
        check_edit_role(&timed_out, &server, later, edited, ATTACH | VIEW, &limits),
        Ok(())
    );

    // AttachFiles without SendMessages is not held (implied restriction).
    let mut silent = h4_ladder();
    silent.everyone.permissions = VIEW;
    let silent_roles = silent.held(3, false);
    let actor = MemberView::new(ACTOR, &silent_roles);
    assert_eq!(
        check_edit_role(&actor, &silent.server(), NOW, silent.at(1), ATTACH, &limits),
        Err(Denied::PermissionEscalation(ATTACH))
    );
}

#[test]
fn an_administrator_grants_any_bit_but_only_below_their_rank() {
    let mut ladder = Ladder::plain();
    ladder.rungs[2].permissions = ADMIN;
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let admin = MemberView::new(ACTOR, &roles);
    let big = RoleLimits::UNLIMITED;

    assert_eq!(
        check_edit_role(&admin, &server, NOW, ladder.at(2), Permissions::ALL, &big),
        Ok(())
    );
    assert_eq!(
        check_create_role(&admin, &server, NOW, 2, Permissions::ALL, &big),
        Ok(())
    );
    // H9: at their own position and above, an administrator is like anyone.
    assert_eq!(
        check_edit_role(&admin, &server, NOW, ladder.at(3), NONE, &RoleLimits::NONE),
        Err(Denied::RoleNotBelowActor)
    );
    assert_eq!(
        check_edit_role(&admin, &server, NOW, ladder.at(4), ADMIN, &RoleLimits::NONE),
        Err(Denied::RoleNotBelowActor)
    );
    assert_eq!(
        check_create_role(&admin, &server, NOW, 3, NONE, &RoleLimits::NONE),
        Err(Denied::RoleNotBelowActor)
    );
}

#[test]
fn a_new_role_holds_only_what_its_creator_holds() {
    let ladder = h4_ladder();
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let limits = RoleLimits::NONE;

    let cases: &[(&str, u32, Permissions, Result<(), Denied>)] = &[
        ("empty role", 1, NONE, Ok(())),
        ("held bits", 2, VIEW | KICK | MANAGE_ROLES, Ok(())),
        (
            "a bit the creator lacks",
            1,
            VIEW | BAN,
            Err(Denied::PermissionEscalation(BAN)),
        ),
        (
            "Administrator",
            1,
            ADMIN,
            Err(Denied::PermissionEscalation(ADMIN)),
        ),
        // Rank is checked before escalation.
        (
            "too high and too much",
            3,
            ADMIN,
            Err(Denied::RoleNotBelowActor),
        ),
        (
            "position 0 and too much",
            0,
            ADMIN,
            Err(Denied::EveryoneRole),
        ),
    ];
    for &(name, position, mask, expected) in cases {
        assert_eq!(
            check_create_role(&actor, &server, NOW, position, mask, &limits),
            expected,
            "{name}"
        );
    }
}

#[test]
fn assigning_a_role_hands_out_its_permissions_and_limits() {
    let mut ladder = h4_ladder();
    ladder.everyone.limits = RoleLimits::NONE.with(LimitKind::MaxUploadSize, 50);
    ladder.rungs[2].limits = RoleLimits::NONE.with(LimitKind::MaxVideoHeight, 1080);
    ladder.rungs[0].permissions = KICK | SEND; // held by the actor
    ladder.rungs[0].limits = RoleLimits::NONE
        .with(LimitKind::MaxUploadSize, 50)
        .with(LimitKind::MaxVideoHeight, 1080);
    ladder.rungs[1].permissions = KICK | SPEAK; // Speak is not held
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let target = MemberView::new(TARGET, &[]);

    assert_eq!(
        check_assign_role(&actor, &target, &server, NOW, ladder.at(1)),
        Ok(())
    );
    assert_eq!(
        check_assign_role(&actor, &actor, &server, NOW, ladder.at(1)),
        Ok(())
    );
    assert_eq!(
        check_assign_role(&actor, &target, &server, NOW, ladder.at(2)),
        Err(Denied::PermissionEscalation(SPEAK))
    );
    // Above all, a member cannot give themself a lower role that carries
    // more than they hold.
    assert_eq!(
        check_assign_role(&actor, &actor, &server, NOW, ladder.at(2)),
        Err(Denied::PermissionEscalation(SPEAK))
    );
    // Taking it away is not an escalation.
    assert_eq!(
        check_revoke_role(&actor, &target, &server, ladder.at(2)),
        Ok(())
    );
    assert_eq!(
        check_revoke_role(&actor, &actor, &server, ladder.at(2)),
        Ok(())
    );

    // Limits: one above the actor's own is refused.
    let mut richer = *ladder.at(1);
    richer.limits.max_video_height = 1081;
    assert_eq!(
        check_assign_role(&actor, &target, &server, NOW, &richer),
        Err(Denied::LimitEscalation(LimitKind::MaxVideoHeight))
    );

    // Rank is checked before escalation.
    let equal_roles = ladder.held(3, false);
    let equal = MemberView::new(TARGET, &equal_roles);
    assert_eq!(
        check_assign_role(&actor, &equal, &server, NOW, ladder.at(2)),
        Err(Denied::EqualRank)
    );
}

// ---------------------------------------------------------------------------
// H4: no escalation through limits
// ---------------------------------------------------------------------------

#[test]
fn a_limit_is_granted_only_up_to_the_actors_own() {
    let mut ladder = Ladder::new(VIEW, MANAGE_ROLES);
    ladder.everyone.limits = RoleLimits::NONE
        .with(LimitKind::MaxVideoBitrate, 8000)
        .with(LimitKind::MaxUploadSize, 50);
    // The actor's top role raises the bitrate.
    ladder.rungs[2].limits = RoleLimits::NONE.with(LimitKind::MaxVideoBitrate, 12_000);
    // The role under edit holds a quota far above the actor's own (0).
    ladder.rungs[0].limits = RoleLimits::NONE
        .with(LimitKind::MaxVideoBitrate, 4000)
        .with(LimitKind::DailyUploadQuota, 900);
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let edited = *ladder.at(1);
    let old = edited.limits;
    let escalation = |kind| Err(Denied::LimitEscalation(kind));

    let cases: &[(&str, RoleLimits, Result<(), Denied>)] = &[
        ("unchanged", old, Ok(())),
        (
            "raise to below the actor's own",
            old.with(LimitKind::MaxVideoBitrate, 11_999),
            Ok(()),
        ),
        (
            "raise to exactly the actor's own",
            old.with(LimitKind::MaxVideoBitrate, 12_000),
            Ok(()),
        ),
        (
            "raise to one above the actor's own",
            old.with(LimitKind::MaxVideoBitrate, 12_001),
            escalation(LimitKind::MaxVideoBitrate),
        ),
        ("lower", old.with(LimitKind::MaxVideoBitrate, 0), Ok(())),
        (
            "a limit from @everyone counts",
            old.with(LimitKind::MaxUploadSize, 50),
            Ok(()),
        ),
        (
            "one above the limit from @everyone",
            old.with(LimitKind::MaxUploadSize, 51),
            escalation(LimitKind::MaxUploadSize),
        ),
        (
            "a limit the actor does not have at all",
            old.with(LimitKind::MaxVideoFps, 1),
            escalation(LimitKind::MaxVideoFps),
        ),
        (
            "a value above the actor's own may stay",
            old.with(LimitKind::MaxVideoBitrate, 5000),
            Ok(()),
        ),
        (
            "but it cannot move to another value above",
            old.with(LimitKind::DailyUploadQuota, 899),
            escalation(LimitKind::DailyUploadQuota),
        ),
        (
            "it can drop to within the actor's own",
            old.with(LimitKind::DailyUploadQuota, 0),
            Ok(()),
        ),
        (
            "the first offending limit is reported",
            old.with(LimitKind::MaxPublishedVideoTracks, 9)
                .with(LimitKind::MaxVideoHeight, 9),
            escalation(LimitKind::MaxVideoHeight),
        ),
    ];
    for &(name, new_limits, expected) in cases {
        let server = ladder.server();
        assert_eq!(
            check_edit_role(
                &actor,
                &server,
                NOW,
                &edited,
                edited.permissions,
                &new_limits
            ),
            expected,
            "{name}"
        );
        // The server-wide cap bounds what is applied, not what may be
        // granted: a cap below the actor's own changes no answer.
        let capped = ServerView {
            limit_caps: RoleLimits::NONE,
            ..server
        };
        assert_eq!(
            check_edit_role(
                &actor,
                &capped,
                NOW,
                &edited,
                edited.permissions,
                &new_limits
            ),
            expected,
            "{name}, capped server"
        );
    }

    // A new role starts from zero.
    let server = ladder.server();
    let within = RoleLimits::NONE.with(LimitKind::MaxVideoBitrate, 12_000);
    let beyond = RoleLimits::NONE.with(LimitKind::MaxVideoBitrate, 12_001);
    assert_eq!(
        check_create_role(&actor, &server, NOW, 1, NONE, &within),
        Ok(())
    );
    assert_eq!(
        check_create_role(&actor, &server, NOW, 1, NONE, &beyond),
        Err(Denied::LimitEscalation(LimitKind::MaxVideoBitrate))
    );
}

#[test]
fn administrators_and_the_owner_are_not_bounded_by_their_role_limits() {
    let mut ladder = Ladder::plain();
    ladder.rungs[2].permissions = ADMIN;
    let server = ServerView {
        limit_caps: RoleLimits::EVERYONE_DEFAULT,
        ..ladder.server()
    };
    let roles = ladder.held(3, false);
    let admin = MemberView::new(ACTOR, &roles);
    let owner = MemberView::new(OWNER, &[]);
    let huge = RoleLimits::UNLIMITED;

    assert_eq!(
        check_edit_role(&admin, &server, NOW, ladder.at(1), NONE, &huge),
        Ok(())
    );
    assert_eq!(
        check_create_role(&admin, &server, NOW, 1, NONE, &huge),
        Ok(())
    );
    assert_eq!(
        check_edit_role(&owner, &server, NOW, ladder.at(5), NONE, &huge),
        Ok(())
    );
}

// ---------------------------------------------------------------------------
// H6, H4, H7: overwrites
// ---------------------------------------------------------------------------

/// `@everyone` gives `VIEW | SEND | CONNECT`; every role adds
/// `MANAGE_ROLES | PIN`.
fn overwrite_ladder() -> Ladder {
    Ladder::new(VIEW | SEND | CONNECT, MANAGE_ROLES | PIN)
}

#[test]
fn an_overwrite_is_edited_only_for_a_target_below_the_actor() {
    let ladder = overwrite_ladder();
    let server = ladder.server();
    let channel = ChannelView::new(CHANNEL, &[]);

    for actor_top in 1..=TOP {
        let roles = ladder.held(actor_top, true);
        let actor = MemberView::new(ACTOR, &roles);

        for position in 0..=TOP {
            let subject = OverwriteSubject::Role(ladder.at(position));
            let expected = expect_below(position, actor_top, Denied::OverwriteTargetNotBelowActor);
            let context = format!("actor top {actor_top}, role at {position}");
            let set = check_set_overwrite(&actor, &server, &channel, NOW, subject, PIN, SEND);
            assert_eq!(set.map(|_| ()), expected, "set: {context}");
            assert_eq!(
                check_delete_overwrite(&actor, &server, &channel, NOW, subject),
                expected,
                "delete: {context}"
            );
        }

        for target_top in 0..=TOP {
            let target_roles = ladder.held(target_top, false);
            let target = MemberView::new(TARGET, &target_roles);
            let subject = OverwriteSubject::Member(&target);
            let expected = match target_top.cmp(&actor_top) {
                std::cmp::Ordering::Less => Ok(()),
                std::cmp::Ordering::Equal => Err(Denied::EqualRank),
                std::cmp::Ordering::Greater => Err(Denied::OverwriteTargetNotBelowActor),
            };
            let context = format!("actor top {actor_top}, member with top {target_top}");
            let set = check_set_overwrite(&actor, &server, &channel, NOW, subject, PIN, SEND);
            assert_eq!(set.map(|_| ()), expected, "set: {context}");
            assert_eq!(
                check_delete_overwrite(&actor, &server, &channel, NOW, subject),
                expected,
                "delete: {context}"
            );
        }

        // Nobody edits their own member overwrite.
        let own = OverwriteSubject::Member(&actor);
        assert_eq!(
            check_set_overwrite(&actor, &server, &channel, NOW, own, NONE, SEND).map(|_| ()),
            Err(Denied::EqualRank),
            "actor top {actor_top}, own overwrite"
        );
    }
    assert_eq!(Denied::OverwriteTargetNotBelowActor.rule(), Some(Rule::H6));
}

#[test]
fn editing_overwrites_needs_manage_roles_in_that_channel() {
    let ladder = overwrite_ladder();
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let subject = OverwriteSubject::Role(ladder.at(1));
    let try_in = |overwrites: &[Overwrite]| {
        let channel = ChannelView::new(CHANNEL, overwrites);
        check_set_overwrite(&actor, &server, &channel, NOW, subject, PIN, NONE).map(|_| ())
    };

    assert_eq!(try_in(&[]), Ok(()), "held server-wide");
    assert_eq!(
        try_in(&[role_overwrite(ladder.at(3).id, NONE, MANAGE_ROLES)]),
        Err(Denied::ManageRolesRequired),
        "denied to the actor's role in this channel"
    );
    assert_eq!(
        try_in(&[member_overwrite(ACTOR, NONE, MANAGE_ROLES)]),
        Err(Denied::ManageRolesRequired),
        "denied to the actor in this channel"
    );
    assert_eq!(
        try_in(&[role_overwrite(EVERYONE, NONE, VIEW)]),
        Err(Denied::ManageRolesRequired),
        "the actor cannot see the channel"
    );
    assert_eq!(Denied::ManageRolesRequired.rule(), Some(Rule::H6));

    // The other way round: no ManageRoles server-wide, allowed in the
    // channel by an overwrite.
    let plain = Ladder::new(VIEW | PIN, NONE);
    let plain_roles = plain.held(3, false);
    let actor = MemberView::new(ACTOR, &plain_roles);
    let subject = OverwriteSubject::Role(plain.at(1));
    let no_overwrites = ChannelView::new(CHANNEL, &[]);
    assert_eq!(
        check_set_overwrite(
            &actor,
            &plain.server(),
            &no_overwrites,
            NOW,
            subject,
            PIN,
            NONE
        )
        .map(|_| ()),
        Err(Denied::ManageRolesRequired)
    );
    let allowed_here = [member_overwrite(ACTOR, MANAGE_ROLES, NONE)];
    let channel = ChannelView::new(CHANNEL, &allowed_here);
    assert_eq!(
        check_set_overwrite(&actor, &plain.server(), &channel, NOW, subject, PIN, NONE).map(|_| ()),
        Ok(())
    );
}

#[test]
fn an_overwrite_changes_only_in_bits_the_actor_holds_in_the_channel() {
    let ladder = overwrite_ladder();
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let low = ladder.at(1);
    let subject = OverwriteSubject::Role(low);
    let escalation = |bits| Err(Denied::PermissionEscalation(bits));

    // In this channel the actor holds VIEW, SEND, CONNECT, MANAGE_ROLES and
    // PIN from their roles and SPEAK through a member overwrite. They hold
    // neither ATTACH nor SCREEN.
    let existing = [
        member_overwrite(ACTOR, SPEAK, SCREEN),
        // The overwrite under edit allows ATTACH, which the actor lacks,
        // and denies SPEAK, which the actor holds.
        role_overwrite(low.id, ATTACH, SPEAK),
    ];
    let channel = ChannelView::new(CHANNEL, &existing);

    let cases: &[(&str, Permissions, Permissions, Result<(), Denied>)] = &[
        ("unchanged", ATTACH, SPEAK, Ok(())),
        ("allow a held bit", ATTACH | PIN, SPEAK, Ok(())),
        (
            "allow a bit held only through an overwrite",
            ATTACH | SPEAK,
            NONE,
            Ok(()),
        ),
        ("deny a held bit", ATTACH, SPEAK | SEND, Ok(())),
        ("lift a denial of a held bit", ATTACH, NONE, Ok(())),
        (
            "allow a bit the actor lacks here",
            ATTACH | SCREEN,
            SPEAK,
            escalation(SCREEN),
        ),
        (
            "deny a bit the actor lacks here",
            ATTACH,
            SPEAK | SCREEN,
            escalation(SCREEN),
        ),
        (
            "drop an allow of a bit the actor lacks",
            NONE,
            SPEAK,
            escalation(ATTACH),
        ),
        (
            "turn an allow the actor lacks into a deny",
            NONE,
            SPEAK | ATTACH,
            escalation(ATTACH),
        ),
        (
            "several",
            ATTACH | SCREEN | PIN,
            SPEAK | Permissions::MOVE_MEMBERS,
            escalation(SCREEN | Permissions::MOVE_MEMBERS),
        ),
    ];
    for &(name, allow, deny, expected) in cases {
        let result = check_set_overwrite(&actor, &server, &channel, NOW, subject, allow, deny);
        assert_eq!(result.map(|_| ()), expected, "{name}");
        if let Ok(stored) = result {
            assert_eq!(stored.target(), OverwriteTarget::Role(low.id), "{name}");
            assert_eq!((stored.allow(), stored.deny()), (allow, deny), "{name}");
        }
    }

    // Deleting drops the allow of ATTACH, which the actor does not hold.
    assert_eq!(
        check_delete_overwrite(&actor, &server, &channel, NOW, subject),
        escalation(ATTACH)
    );
}

#[test]
fn lifting_a_denial_needs_the_bit() {
    // The hole this closes: a role holds Speak server-wide and the channel
    // denies it to that role. A manager who cannot speak here must not be
    // able to hand Speak out by deleting the denial.
    let mut ladder = Ladder::new(VIEW | CONNECT, NONE);
    ladder.rungs[0].permissions = SPEAK;
    ladder.rungs[2].permissions = MANAGE_ROLES;
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let speakers = ladder.at(1);
    let subject = OverwriteSubject::Role(speakers);
    let existing = [role_overwrite(speakers.id, NONE, SPEAK)];
    let channel = ChannelView::new(CHANNEL, &existing);

    assert_eq!(
        check_delete_overwrite(&actor, &server, &channel, NOW, subject),
        Err(Denied::PermissionEscalation(SPEAK))
    );
    assert_eq!(
        check_set_overwrite(&actor, &server, &channel, NOW, subject, NONE, NONE).map(|_| ()),
        Err(Denied::PermissionEscalation(SPEAK))
    );

    // A manager who can speak in the channel may lift it.
    ladder.rungs[2].permissions = MANAGE_ROLES | SPEAK;
    let server = ladder.server();
    let roles = ladder.held(3, false);
    let actor = MemberView::new(ACTOR, &roles);
    let subject = OverwriteSubject::Role(ladder.at(1));
    assert_eq!(
        check_delete_overwrite(&actor, &server, &channel, NOW, subject),
        Ok(())
    );
}

#[test]
fn an_overwrite_must_be_valid_whoever_writes_it() {
    let ladder = Ladder::new(VIEW, MANAGE_ROLES | ADMIN);
    let server = ladder.server();
    let top_roles = ladder.held(TOP, false);
    let owner = MemberView::new(OWNER, &[]);
    let admin = MemberView::new(ACTOR, &top_roles);
    let subject = OverwriteSubject::Role(ladder.at(1));
    let empty = ChannelView::new(CHANNEL, &[]);
    // A stored overwrite that already carries a server-scope bit, as old
    // data might.
    let legacy = [role_overwrite(ladder.at(1).id, KICK, NONE)];
    let with_legacy = ChannelView::new(CHANNEL, &legacy);

    for (name, actor) in [("owner", &owner), ("administrator", &admin)] {
        let set = |channel: &ChannelView<'_>, allow, deny| {
            check_set_overwrite(actor, &server, channel, NOW, subject, allow, deny).map(|_| ())
        };
        assert_eq!(
            set(&empty, SPEAK | PIN, PIN | SEND),
            Err(Denied::OverlappingOverwrite(PIN)),
            "{name}: overlap"
        );
        assert_eq!(
            set(&empty, SPEAK | KICK, NONE),
            Err(Denied::ServerScopeInOverwrite(KICK)),
            "{name}: server-scope allow"
        );
        assert_eq!(
            set(&empty, SPEAK, BAN | ADMIN),
            Err(Denied::ServerScopeInOverwrite(BAN | ADMIN)),
            "{name}: server-scope deny"
        );
        // A server-scope bit that old data carries does not block other
        // edits and can be cleaned up, but it cannot move to the other mask.
        assert_eq!(
            set(&with_legacy, KICK | SPEAK, NONE),
            Ok(()),
            "{name}: legacy bit kept"
        );
        assert_eq!(
            set(&with_legacy, SPEAK, NONE),
            Ok(()),
            "{name}: legacy bit removed"
        );
        assert_eq!(
            set(&with_legacy, SPEAK, KICK),
            Err(Denied::ServerScopeInOverwrite(KICK)),
            "{name}: legacy bit moved to deny"
        );
        assert_eq!(
            set(&empty, SPEAK | BIT_46, VIEW),
            Ok(()),
            "{name}: reserved bits are channel-scope"
        );
    }
    assert_eq!(Denied::OverlappingOverwrite(PIN).rule(), None);
    assert_eq!(Denied::ServerScopeInOverwrite(KICK).rule(), None);
}

// ---------------------------------------------------------------------------
// The typed reason
// ---------------------------------------------------------------------------

#[test]
fn every_refusal_names_its_rule() {
    let cases: &[(Denied, Option<Rule>, &str)] = &[
        (Denied::MissingPermissions(KICK), None, ""),
        (Denied::OwnerIsTarget, Some(Rule::H1), "H1"),
        (Denied::RoleNotBelowActor, Some(Rule::H2), "H2"),
        (Denied::AssignedRoleNotBelowActor, Some(Rule::H3), "H3"),
        (Denied::AssignTargetNotBelowActor, Some(Rule::H3), "H3"),
        (Denied::PermissionEscalation(KICK), Some(Rule::H4), "H4"),
        (
            Denied::LimitEscalation(LimitKind::MaxUploadSize),
            Some(Rule::H4),
            "H4",
        ),
        (Denied::TargetNotBelowActor, Some(Rule::H5), "H5"),
        (Denied::ManageRolesRequired, Some(Rule::H6), "H6"),
        (Denied::OverwriteTargetNotBelowActor, Some(Rule::H6), "H6"),
        (Denied::EqualRank, Some(Rule::H7), "H7"),
        (Denied::EveryoneRole, Some(Rule::H8), "H8"),
        (Denied::OwnerCannotLeave, Some(Rule::H10), "H10"),
        (Denied::ServerScopeInOverwrite(KICK), None, ""),
        (Denied::OverlappingOverwrite(KICK), None, ""),
    ];
    for &(denied, rule, name) in cases {
        assert_eq!(denied.rule(), rule, "{denied:?}");
        assert_eq!(rule.map_or("", Rule::name), name, "{denied:?}");

        let text = denied.to_string();
        assert!(!text.is_empty(), "{denied:?}");
        assert!(text.is_ascii(), "{denied:?}: {text}");
        assert!(!text.ends_with('.'), "{denied:?}: {text}");
    }
    assert_eq!(
        Denied::MissingPermissions(KICK | SPEAK).to_string(),
        "missing permission: KickMembers | Speak"
    );
    assert_eq!(
        Denied::LimitEscalation(LimitKind::MaxUploadSize).to_string(),
        "the limit max_upload_size is above what the actor may grant"
    );
}
