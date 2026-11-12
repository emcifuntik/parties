//! Model-based test of rule H4, "no escalation" (section 14): random
//! sequences of operations on a small server. Every operation is put to the
//! `check_*` function the server will call; what the check permits is
//! applied to the model, and after each permitted operation the invariants
//! below must hold.
//!
//! The permission a request needs before the hierarchy is looked at
//! (`ManageRoles` for the role requests) is not required here. That only
//! makes more sequences permitted, so the test is stronger without it.
//!
//! For the member who performed the operation ("the actor"):
//!
//! - **I1** The actor's server-wide effective permissions, applied limits
//!   and grant ceiling do not grow.
//! - **I2** The actor's effective permissions in a channel do not grow,
//!   unless the operation changed the actor's own set of roles. (Taking a
//!   role, dropping one or deleting one can open a channel through that
//!   role's overwrites. The hierarchy governs that, not H4.)
//! - **I3** Nobody gains what the actor does not hold: a base permission a
//!   member gains is one the actor held server-wide; a limit a member gains
//!   is at most the actor's own; and after an overwrite edit, a bit a member
//!   gains in that channel before the implied restrictions is one the actor
//!   held there.
//! - **I4** Rank is untouched: the set of members at or above the actor
//!   stays the same, and so does the actor's top role.
//! - **I5** Only an administrator makes an administrator.

mod common;

use common::{Channel, Member, World, config, limits, permissions, world};
use parties_perm::{
    Denied, LimitKind, MemberView, OverwriteSubject, OverwriteTarget, Permissions, Role, RoleId,
    RoleLimits, ServerView, check_assign_role, check_create_role, check_delete_overwrite,
    check_delete_role, check_edit_role, check_move_role, check_revoke_role, check_set_overwrite,
};
use proptest::prelude::*;
use proptest::sample::Index;
use proptest::strategy::ValueTree;
use proptest::test_runner::{TestCaseError, TestRunner};

#[derive(Clone, Copy, Debug)]
enum Subject {
    Role(Index),
    Member(Index),
}

#[derive(Clone, Debug)]
enum Op {
    CreateRole {
        position: Index,
        mask: Permissions,
        limits: RoleLimits,
    },
    EditRole {
        role: Index,
        toggled: Permissions,
        limits: Option<RoleLimits>,
    },
    DeleteRole {
        role: Index,
    },
    MoveRole {
        role: Index,
        position: Index,
    },
    AssignRole {
        member: Index,
        role: Index,
    },
    RevokeRole {
        member: Index,
        role: Index,
    },
    SetOverwrite {
        channel: Index,
        subject: Subject,
        allow: Permissions,
        deny: Permissions,
    },
    DeleteOverwrite {
        channel: Index,
        subject: Subject,
    },
}

const KINDS: usize = 8;
const KIND_NAMES: [&str; KINDS] = [
    "CreateRole",
    "EditRole",
    "DeleteRole",
    "MoveRole",
    "AssignRole",
    "RevokeRole",
    "SetOverwrite",
    "DeleteOverwrite",
];

impl Op {
    fn kind(&self) -> usize {
        match self {
            Op::CreateRole { .. } => 0,
            Op::EditRole { .. } => 1,
            Op::DeleteRole { .. } => 2,
            Op::MoveRole { .. } => 3,
            Op::AssignRole { .. } => 4,
            Op::RevokeRole { .. } => 5,
            Op::SetOverwrite { .. } => 6,
            Op::DeleteOverwrite { .. } => 7,
        }
    }
}

/// One operation by one member. A `plausible` step trims its request to
/// what the actor holds, so that it has a fair chance to be permitted; the
/// others ask for whatever was drawn and are mostly refused.
#[derive(Clone, Debug)]
struct Step {
    actor: Index,
    plausible: bool,
    op: Op,
}

fn subject() -> impl Strategy<Value = Subject> {
    prop_oneof![
        any::<Index>().prop_map(Subject::Role),
        any::<Index>().prop_map(Subject::Member)
    ]
}

fn op() -> impl Strategy<Value = Op> {
    prop_oneof![
        2 => (any::<Index>(), permissions(), limits())
            .prop_map(|(position, mask, limits)| Op::CreateRole { position, mask, limits }),
        4 => (any::<Index>(), permissions(), proptest::option::of(limits()))
            .prop_map(|(role, toggled, limits)| Op::EditRole { role, toggled, limits }),
        1 => any::<Index>().prop_map(|role| Op::DeleteRole { role }),
        2 => any::<(Index, Index)>().prop_map(|(role, position)| Op::MoveRole { role, position }),
        4 => any::<(Index, Index)>().prop_map(|(member, role)| Op::AssignRole { member, role }),
        3 => any::<(Index, Index)>().prop_map(|(member, role)| Op::RevokeRole { member, role }),
        6 => (any::<Index>(), subject(), permissions(), permissions())
            .prop_map(|(channel, subject, allow, deny)| Op::SetOverwrite { channel, subject, allow, deny }),
        4 => (any::<Index>(), subject())
            .prop_map(|(channel, subject)| Op::DeleteOverwrite { channel, subject }),
    ]
}

/// A generated server. In a good half of them `@everyone` can see the
/// channels and manage roles, so that ordinary members get to edit
/// overwrites often enough to exercise the channel invariants.
fn model_world() -> impl Strategy<Value = World> {
    (world(), proptest::bool::weighted(0.6)).prop_map(|(mut world, everyone_manages)| {
        if everyone_manages {
            world.everyone.permissions |= Permissions::VIEW_CHANNEL | Permissions::MANAGE_ROLES;
        }
        world
    })
}

fn steps() -> impl Strategy<Value = Vec<Step>> {
    let step =
        (any::<Index>(), proptest::bool::weighted(0.8), op()).prop_map(|(actor, plausible, op)| {
            Step {
                actor,
                plausible,
                op,
            }
        });
    proptest::collection::vec(step, 1..40)
}

/// Everything the invariants compare, taken before and after an operation.
/// The vectors are indexed by member and, where nested, by channel; no
/// operation adds or removes members or channels.
struct Snapshot {
    server_wide: Vec<Permissions>,
    base: Vec<Permissions>,
    own_limits: Vec<RoleLimits>,
    applied_limits: Vec<RoleLimits>,
    ceiling: Vec<RoleLimits>,
    in_channel: Vec<Vec<Permissions>>,
    raw_in_channel: Vec<Vec<Permissions>>,
    admin: Vec<bool>,
    at_or_above_actor: Vec<bool>,
    actor_top_role: Option<RoleId>,
    actor_roles: Vec<RoleId>,
}

impl Snapshot {
    fn take(world: &World, actor: usize) -> Self {
        fn per_member<T>(world: &World, f: impl Fn(&Member) -> T) -> Vec<T> {
            world.members.iter().map(f).collect()
        }
        fn per_channel(
            world: &World,
            f: impl Fn(&Member, &Channel) -> Permissions,
        ) -> Vec<Vec<Permissions>> {
            per_member(world, |member| {
                world
                    .channels
                    .iter()
                    .map(|channel| f(member, channel))
                    .collect()
            })
        }
        let actor_top = world.top(&world.members[actor]);
        let mut actor_roles = world.members[actor].roles.clone();
        actor_roles.sort();
        Self {
            server_wide: per_member(world, |member| world.effective(member, None)),
            base: per_member(world, |member| world.base(member)),
            own_limits: per_member(world, |member| world.role_limits(member)),
            applied_limits: per_member(world, |member| world.effective_limits(member, None)),
            ceiling: per_member(world, |member| world.ceiling(member)),
            in_channel: per_channel(world, |member, channel| {
                world.effective(member, Some(channel))
            }),
            raw_in_channel: per_channel(world, |member, channel| world.raw_in(member, channel)),
            admin: per_member(world, |member| world.is_admin(member)),
            at_or_above_actor: world
                .members
                .iter()
                .map(|member| member.id == world.owner || world.top(member) >= actor_top)
                .collect(),
            actor_top_role: world
                .roles_of(&world.members[actor])
                .into_iter()
                .max_by_key(|role| role.position)
                .map(|role| role.id),
            actor_roles,
        }
    }
}

fn at_most(a: &RoleLimits, b: &RoleLimits) -> bool {
    LimitKind::ALL
        .into_iter()
        .all(|kind| a.get(kind) <= b.get(kind))
}

fn check_invariants(
    before: &Snapshot,
    after: &Snapshot,
    actor: usize,
    step: &Step,
) -> Result<(), TestCaseError> {
    let is_overwrite_op = matches!(
        step.op,
        Op::SetOverwrite { .. } | Op::DeleteOverwrite { .. }
    );
    let actor_roles_changed = before.actor_roles != after.actor_roles;

    // I1
    prop_assert!(
        before.server_wide[actor].contains(after.server_wide[actor]),
        "I1: the actor gained {} server-wide by {:?}",
        after.server_wide[actor] - before.server_wide[actor],
        step
    );
    prop_assert!(
        at_most(&after.applied_limits[actor], &before.applied_limits[actor]),
        "I1: the actor's applied limits grew by {:?}",
        step
    );
    prop_assert!(
        at_most(&after.ceiling[actor], &before.ceiling[actor]),
        "I1: the actor's grant ceiling grew by {:?}",
        step
    );

    // I2
    if !actor_roles_changed {
        for (channel, (now, was)) in after.in_channel[actor]
            .iter()
            .zip(&before.in_channel[actor])
            .enumerate()
        {
            prop_assert!(
                was.contains(*now),
                "I2: the actor gained {} in channel {} by {:?}",
                *now - *was,
                channel,
                step
            );
        }
    }

    // I3
    let held = before.server_wide[actor];
    for member in 0..before.base.len() {
        let gained = after.base[member] - before.base[member];
        prop_assert!(
            held.contains(gained),
            "I3: member {} gained {} that the actor does not hold, by {:?}",
            member,
            gained - held,
            step
        );
        let bound = before.own_limits[member].max(before.ceiling[actor]);
        prop_assert!(
            at_most(&after.own_limits[member], &bound),
            "I3: member {} gained a limit above the actor's own by {:?}",
            member,
            step
        );
        if is_overwrite_op {
            let channels = after.raw_in_channel[member]
                .iter()
                .zip(&before.raw_in_channel[member]);
            for (channel, (now, was)) in channels.enumerate() {
                let gained = *now - *was;
                let held_here = before.in_channel[actor][channel];
                prop_assert!(
                    held_here.contains(gained),
                    "I3: member {} gained {} in channel {} that the actor does not hold there, by {:?}",
                    member,
                    gained - held_here,
                    channel,
                    step
                );
            }
        }
    }

    // I4
    prop_assert_eq!(
        &after.at_or_above_actor,
        &before.at_or_above_actor,
        "I4: rank changed by {:?}",
        step
    );
    prop_assert_eq!(
        after.actor_top_role,
        before.actor_top_role,
        "I4: the actor's top role changed by {:?}",
        step
    );

    // I5
    for member in 0..before.admin.len() {
        prop_assert!(
            !after.admin[member] || before.admin[member] || before.admin[actor],
            "I5: member {} became an administrator by {:?}",
            member,
            step
        );
    }
    prop_assert_eq!(
        after.server_wide[0],
        Permissions::ALL,
        "the owner lost something"
    );
    Ok(())
}

/// The model's own consistency: unique dense positions, known role ids, at
/// most one overwrite per target.
fn check_model(world: &World) -> Result<(), TestCaseError> {
    for (index, role) in world.roles.iter().enumerate() {
        prop_assert_eq!(role.position as usize, index + 1);
        prop_assert!(role.id != world.everyone.id);
        prop_assert!(world.roles[..index].iter().all(|other| other.id != role.id));
    }
    for member in &world.members {
        prop_assert_eq!(
            world.roles_of(member).len(),
            member.roles.len(),
            "a member holds a deleted role"
        );
    }
    for channel in &world.channels {
        for (index, overwrite) in channel.overwrites.iter().enumerate() {
            prop_assert!(!overwrite.is_empty());
            prop_assert!(
                channel.overwrites[..index]
                    .iter()
                    .all(|other| other.target() != overwrite.target())
            );
            if let OverwriteTarget::Role(role) = overwrite.target() {
                prop_assert!(
                    role == world.everyone.id || world.roles.iter().any(|known| known.id == role),
                    "an overwrite names a deleted role"
                );
            }
        }
    }
    Ok(())
}

/// Any role of the world by index: 0 is `@everyone`.
fn role_at(world: &World, pick: Index) -> (usize, Role) {
    let index = pick.index(world.roles.len() + 1);
    (
        index,
        if index == 0 {
            world.everyone
        } else {
            world.roles[index - 1]
        },
    )
}

/// Calls `f` with the views of the acting member and the server.
fn with_actor<T>(
    world: &World,
    actor: usize,
    f: impl FnOnce(&MemberView<'_>, &ServerView<'_>) -> T,
) -> T {
    let member = &world.members[actor];
    let roles = world.roles_of(member);
    f(&member.view(&roles), &world.server())
}

/// Calls `f` with the views of the acting member, another member and the
/// server.
fn with_actor_and_target<T>(
    world: &World,
    actor: usize,
    target: usize,
    f: impl FnOnce(&MemberView<'_>, &MemberView<'_>, &ServerView<'_>) -> T,
) -> T {
    let target = &world.members[target];
    let target_roles = world.roles_of(target);
    let target_view = target.view(&target_roles);
    with_actor(world, actor, |actor, server| f(actor, &target_view, server))
}

/// Puts the step to the crate's check and, when permitted, applies it.
/// Returns whether it was permitted, or `None` when the step does not apply
/// to this world (an overwrite step on a server without channels).
fn apply(
    world: &mut World,
    actor: usize,
    step: &Step,
    next_role_id: &mut u64,
) -> Option<Result<(), Denied>> {
    let now = world.now;
    let held = world.effective(&world.members[actor], None);
    let ceiling = world.ceiling(&world.members[actor]);

    let verdict = match &step.op {
        Op::CreateRole {
            position,
            mask,
            limits,
        } => {
            let position = 1 + position.index(world.roles.len() + 1);
            let (mask, limits) = if step.plausible {
                (*mask & held, limits.min(ceiling))
            } else {
                (*mask, *limits)
            };
            let slot = u32::try_from(position).expect("few roles");
            let verdict = with_actor(world, actor, |actor, server| {
                check_create_role(actor, server, now, slot, mask, &limits)
            });
            if verdict.is_ok() {
                let id = RoleId(*next_role_id);
                *next_role_id += 1;
                world.roles.insert(
                    position - 1,
                    Role {
                        id,
                        position: 0,
                        permissions: mask,
                        limits,
                    },
                );
                world.renumber();
            }
            verdict
        }
        Op::EditRole {
            role,
            toggled,
            limits,
        } => {
            let (index, current) = role_at(world, *role);
            let toggled = if step.plausible {
                *toggled & held
            } else {
                *toggled
            };
            let new_mask = current.permissions ^ toggled;
            let new_limits = match limits {
                None => current.limits,
                Some(limits) if step.plausible => limits.min(ceiling),
                Some(limits) => *limits,
            };
            let verdict = with_actor(world, actor, |actor, server| {
                check_edit_role(actor, server, now, &current, new_mask, &new_limits)
            });
            if verdict.is_ok() {
                let stored = if index == 0 {
                    &mut world.everyone
                } else {
                    &mut world.roles[index - 1]
                };
                stored.permissions = new_mask;
                stored.limits = new_limits;
            }
            verdict
        }
        Op::DeleteRole { role } => {
            let (index, current) = role_at(world, *role);
            let verdict = with_actor(world, actor, |actor, server| {
                check_delete_role(actor, server, &current)
            });
            if verdict.is_ok() {
                world.roles.remove(index - 1);
                world.renumber();
                for member in &mut world.members {
                    member.roles.retain(|held| *held != current.id);
                }
                for channel in &mut world.channels {
                    channel.overwrites.retain(|overwrite| {
                        overwrite.target() != OverwriteTarget::Role(current.id)
                    });
                }
            }
            verdict
        }
        Op::MoveRole { role, position } => {
            let (index, current) = role_at(world, *role);
            let position = position.index(world.roles.len() + 1);
            let slot = u32::try_from(position).expect("few roles");
            let verdict = with_actor(world, actor, |actor, server| {
                check_move_role(actor, server, &current, slot)
            });
            if verdict.is_ok() {
                let moved = world.roles.remove(index - 1);
                world.roles.insert(position - 1, moved);
                world.renumber();
            }
            verdict
        }
        Op::AssignRole { member, role } => {
            let target = member.index(world.members.len());
            let (_, current) = role_at(world, *role);
            let verdict = with_actor_and_target(world, actor, target, |actor, target, server| {
                check_assign_role(actor, target, server, now, &current)
            });
            if verdict.is_ok() && !world.members[target].roles.contains(&current.id) {
                world.members[target].roles.push(current.id);
            }
            verdict
        }
        Op::RevokeRole { member, role } => {
            let target = member.index(world.members.len());
            let (_, current) = role_at(world, *role);
            let verdict = with_actor_and_target(world, actor, target, |actor, target, server| {
                check_revoke_role(actor, target, server, &current)
            });
            if verdict.is_ok() {
                world.members[target]
                    .roles
                    .retain(|held| *held != current.id);
            }
            verdict
        }
        Op::SetOverwrite {
            channel,
            subject,
            allow,
            deny,
        } => {
            if world.channels.is_empty() {
                return None;
            }
            let channel = channel.index(world.channels.len());
            let held_here = world.effective(&world.members[actor], Some(&world.channels[channel]));
            let (subject_role, subject_member) = resolve_subject(world, *subject);
            let target = match subject_role {
                Some(role) => OverwriteTarget::Role(role.id),
                None => OverwriteTarget::Member(world.members[subject_member].id),
            };
            let (old_allow, old_deny) = world.channels[channel].view().masks_for(target);
            let (allow, deny) = if step.plausible {
                // Edit the stored overwrite in channel-scope bits the actor
                // holds here.
                let allow = old_allow ^ ((*allow & held_here) - Permissions::SERVER_SCOPE);
                let deny = (old_deny ^ ((*deny & held_here) - Permissions::SERVER_SCOPE)) - allow;
                (allow, deny)
            } else {
                (*allow, *deny - *allow)
            };
            let verdict =
                with_actor_and_target(world, actor, subject_member, |actor, member, server| {
                    let subject = match &subject_role {
                        Some(role) => OverwriteSubject::Role(role),
                        None => OverwriteSubject::Member(member),
                    };
                    check_set_overwrite(
                        actor,
                        server,
                        &world.channels[channel].view(),
                        now,
                        subject,
                        allow,
                        deny,
                    )
                });
            if let Ok(overwrite) = verdict {
                let overwrites = &mut world.channels[channel].overwrites;
                overwrites.retain(|existing| existing.target() != target);
                if !overwrite.is_empty() {
                    overwrites.push(overwrite);
                }
            }
            verdict.map(|_| ())
        }
        Op::DeleteOverwrite { channel, subject } => {
            if world.channels.is_empty() {
                return None;
            }
            let channel = channel.index(world.channels.len());
            let (subject_role, subject_member) = resolve_subject(world, *subject);
            let target = match subject_role {
                Some(role) => OverwriteTarget::Role(role.id),
                None => OverwriteTarget::Member(world.members[subject_member].id),
            };
            let verdict =
                with_actor_and_target(world, actor, subject_member, |actor, member, server| {
                    let subject = match &subject_role {
                        Some(role) => OverwriteSubject::Role(role),
                        None => OverwriteSubject::Member(member),
                    };
                    check_delete_overwrite(
                        actor,
                        server,
                        &world.channels[channel].view(),
                        now,
                        subject,
                    )
                });
            if verdict.is_ok() {
                world.channels[channel]
                    .overwrites
                    .retain(|existing| existing.target() != target);
            }
            verdict
        }
    };
    Some(verdict)
}

/// The role the subject names, or else the index of the member it names.
/// The member index is always valid; it is only used when there is no role.
fn resolve_subject(world: &World, subject: Subject) -> (Option<Role>, usize) {
    match subject {
        Subject::Role(pick) => (Some(role_at(world, pick).1), 0),
        Subject::Member(pick) => (None, pick.index(world.members.len())),
    }
}

/// How many steps of each kind were permitted and refused, split by whether
/// the actor was an administrator.
#[derive(Default)]
struct Tally {
    permitted_ordinary: [usize; KINDS],
    permitted_admin: [usize; KINDS],
    refused: [usize; KINDS],
}

impl Tally {
    fn add(&mut self, other: &Tally) {
        let pairs = [
            (&mut self.permitted_ordinary, &other.permitted_ordinary),
            (&mut self.permitted_admin, &other.permitted_admin),
            (&mut self.refused, &other.refused),
        ];
        for (sum, counts) in pairs {
            for (sum, count) in sum.iter_mut().zip(counts) {
                *sum += count;
            }
        }
    }
}

/// Runs the steps and returns the server they leave behind. With
/// `fixed_actor`, every step is performed by that member; otherwise each
/// step names its own actor. The owner never acts: the owner passes every
/// check and holds everything.
fn run(
    world: World,
    steps: &[Step],
    fixed_actor: Option<usize>,
) -> Result<(World, Tally), TestCaseError> {
    let mut world = world;
    let mut next_role_id = 1000;
    let mut tally = Tally::default();
    check_model(&world)?;

    for step in steps {
        let actor = fixed_actor.unwrap_or_else(|| 1 + step.actor.index(world.members.len() - 1));
        let admin = world.is_admin(&world.members[actor]);
        let before = Snapshot::take(&world, actor);
        let untouched = world.clone();

        match apply(&mut world, actor, step, &mut next_role_id) {
            None => {}
            Some(Ok(())) => {
                check_model(&world)?;
                let after = Snapshot::take(&world, actor);
                check_invariants(&before, &after, actor, step)?;
                let counter = if admin {
                    &mut tally.permitted_admin
                } else {
                    &mut tally.permitted_ordinary
                };
                counter[step.op.kind()] += 1;
            }
            Some(Err(_)) => {
                // A refused step changes nothing.
                prop_assert_eq!(format!("{world:?}"), format!("{untouched:?}"));
                tally.refused[step.op.kind()] += 1;
            }
        }
    }
    Ok((world, tally))
}

/// Removes `Administrator` from `@everyone` and from every role of the
/// member, so that the member starts as a non-administrator.
fn without_administrator(mut world: World, member: usize) -> World {
    world.everyone.permissions -= Permissions::ADMINISTRATOR;
    let held = world.members[member].roles.clone();
    for role in world
        .roles
        .iter_mut()
        .filter(|role| held.contains(&role.id))
    {
        role.permissions -= Permissions::ADMINISTRATOR;
    }
    world
}

proptest! {
    #![proptest_config(config(512, "tests/regressions/model_h4.txt"))]

    /// Section 14: "For random sequences of permitted operations by a
    /// non-administrator, the actor's effective permissions never gain a
    /// bit." One member performs the whole sequence; at the end they hold
    /// nothing server-wide that they did not hold at the start.
    #[test]
    fn a_non_administrator_never_gains_a_bit_over_a_whole_sequence(
        world in model_world(),
        actor in any::<Index>(),
        steps in steps(),
    ) {
        let actor = 1 + actor.index(world.members.len() - 1);
        let world = without_administrator(world, actor);
        let member = world.members[actor].clone();
        let start = world.effective(&member, None);
        let start_limits = world.effective_limits(&member, None);
        let start_ceiling = world.ceiling(&member);
        prop_assert!(!start.contains(Permissions::ADMINISTRATOR));

        // `run` checks I1 to I5 after every permitted step; a failure there
        // names the step.
        let (end, _) = run(world, &steps, Some(actor))?;

        let member = &end.members[actor];
        let finish = end.effective(member, None);
        prop_assert!(start.contains(finish), "gained {}", finish - start);
        prop_assert!(at_most(&end.effective_limits(member, None), &start_limits));
        prop_assert!(at_most(&end.ceiling(member), &start_ceiling));
        prop_assert!(!end.is_admin(member));
    }

    /// The same invariants with a different actor at every step,
    /// administrators included: they are bound by rank (H9), so I4 holds
    /// for them as well.
    #[test]
    fn every_permitted_operation_keeps_the_invariants(world in model_world(), steps in steps()) {
        run(world, &steps, None)?;
    }
}

/// The generators must actually exercise the rules: over a fixed set of
/// sequences, every kind of operation is permitted to an ordinary member at
/// least once, and refused at least once. Without this, a generator that
/// only ever produces refused requests would make the tests above pass
/// while testing nothing.
#[test]
fn the_sequences_exercise_every_operation() {
    let mut runner = TestRunner::deterministic();
    let strategy = (model_world(), steps());
    let mut total = Tally::default();
    for _ in 0..400 {
        let (world, steps) = strategy.new_tree(&mut runner).expect("a value").current();
        let (_, tally) = run(world, &steps, None).expect("the invariants hold");
        total.add(&tally);
    }
    for (kind, name) in KIND_NAMES.into_iter().enumerate() {
        println!(
            "{name}: permitted to ordinary members {}, to administrators {}, refused {}",
            total.permitted_ordinary[kind], total.permitted_admin[kind], total.refused[kind]
        );
        assert!(
            total.permitted_ordinary[kind] >= 20,
            "{name}: permitted to an ordinary member only {} times",
            total.permitted_ordinary[kind]
        );
        assert!(
            total.permitted_admin[kind] > 0,
            "{name}: never permitted to an administrator"
        );
        assert!(
            total.refused[kind] >= 20,
            "{name}: refused only {} times",
            total.refused[kind]
        );
    }
}
