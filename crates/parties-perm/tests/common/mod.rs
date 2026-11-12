//! Helpers shared by the integration tests: short constructors, an owned
//! model of a server ("world") to borrow views from, and the `proptest`
//! strategies that generate worlds.

// Every test binary uses a different part of this module.
#![allow(dead_code)]

use parties_perm::defaults::EVERYONE_PERMISSIONS;
use parties_perm::{
    ChannelId, ChannelView, LimitKind, MemberView, NewMemberPolicy, Overwrite, OverwriteTarget,
    Permissions, Role, RoleId, RoleLimits, ServerView, Timestamp, UserId, apply_overwrites,
    base_permissions, effective, effective_limits, limit_grant_ceiling, role_limits,
};
use proptest::prelude::*;
use proptest::test_runner::FileFailurePersistence;

/// The instant the tests run at, unless they say otherwise.
pub const NOW: Timestamp = Timestamp(1_000_000);

/// The owner in every hand-written fixture and generated world.
pub const OWNER: UserId = UserId(1);

/// The id of `@everyone` in every hand-written fixture and generated world.
pub const EVERYONE: RoleId = RoleId(100);

/// A role without limits.
pub fn role(id: u64, position: u32, permissions: Permissions) -> Role {
    Role {
        id: RoleId(id),
        position,
        permissions,
        limits: RoleLimits::NONE,
    }
}

/// An overwrite for a role. Panics when the masks overlap.
pub fn role_overwrite(role: RoleId, allow: Permissions, deny: Permissions) -> Overwrite {
    Overwrite::new(OverwriteTarget::Role(role), allow, deny).expect("allow and deny are disjoint")
}

/// An overwrite for a member. Panics when the masks overlap.
pub fn member_overwrite(user: UserId, allow: Permissions, deny: Permissions) -> Overwrite {
    Overwrite::new(OverwriteTarget::Member(user), allow, deny).expect("allow and deny are disjoint")
}

/// A member of a [`World`].
#[derive(Clone, Debug)]
pub struct Member {
    pub id: UserId,
    /// Ids of the member's roles, without `@everyone`, without duplicates.
    pub roles: Vec<RoleId>,
    pub joined_at: Timestamp,
    pub timeout_until: Option<Timestamp>,
    pub speak_grant: Option<ChannelId>,
}

impl Member {
    /// The view of this member over `roles`, which comes from
    /// [`World::roles_of`].
    pub fn view<'a>(&self, roles: &'a [&'a Role]) -> MemberView<'a> {
        MemberView {
            user_id: self.id,
            roles,
            joined_at: self.joined_at,
            timeout_until: self.timeout_until,
            speak_grant: self.speak_grant,
        }
    }
}

/// A channel of a [`World`].
#[derive(Clone, Debug)]
pub struct Channel {
    pub id: ChannelId,
    /// At most one overwrite per target, none of them empty.
    pub overwrites: Vec<Overwrite>,
    pub caps: RoleLimits,
}

impl Channel {
    pub fn view(&self) -> ChannelView<'_> {
        ChannelView {
            id: self.id,
            overwrites: &self.overwrites,
            limit_caps: self.caps,
        }
    }
}

/// An owned server state. `roles` is ordered from the lowest to the highest
/// role and the position of `roles[i]` is `i + 1`. `members[0]` is the
/// owner.
#[derive(Clone, Debug)]
pub struct World {
    pub owner: UserId,
    pub everyone: Role,
    pub roles: Vec<Role>,
    pub members: Vec<Member>,
    pub channels: Vec<Channel>,
    pub caps: RoleLimits,
    pub policy: NewMemberPolicy,
    pub now: Timestamp,
}

impl World {
    pub fn server(&self) -> ServerView<'_> {
        ServerView {
            owner_id: self.owner,
            everyone: &self.everyone,
            limit_caps: self.caps,
            new_members: self.policy,
        }
    }

    /// Makes the positions follow the order of `roles` again.
    pub fn renumber(&mut self) {
        for (index, role) in self.roles.iter_mut().enumerate() {
            role.position = u32::try_from(index).expect("few roles") + 1;
        }
    }

    /// The roles of `member`, resolved. Unknown ids are skipped.
    pub fn roles_of(&self, member: &Member) -> Vec<&Role> {
        member
            .roles
            .iter()
            .filter_map(|id| self.roles.iter().find(|role| role.id == *id))
            .collect()
    }

    pub fn top(&self, member: &Member) -> u32 {
        self.server()
            .top_position(&member.view(&self.roles_of(member)))
    }

    pub fn base(&self, member: &Member) -> Permissions {
        base_permissions(&member.view(&self.roles_of(member)), &self.server())
    }

    pub fn is_admin(&self, member: &Member) -> bool {
        member.id == self.owner || self.base(member).contains(Permissions::ADMINISTRATOR)
    }

    pub fn effective(&self, member: &Member, channel: Option<&Channel>) -> Permissions {
        let roles = self.roles_of(member);
        let channel = channel.map(Channel::view);
        effective(
            &member.view(&roles),
            &self.server(),
            channel.as_ref(),
            self.now,
        )
    }

    /// The member's permissions in `channel` after overwrites and before
    /// the implied restrictions.
    pub fn raw_in(&self, member: &Member, channel: &Channel) -> Permissions {
        let roles = self.roles_of(member);
        let view = member.view(&roles);
        let server = self.server();
        apply_overwrites(
            base_permissions(&view, &server),
            &view,
            &server,
            &channel.view(),
        )
    }

    pub fn role_limits(&self, member: &Member) -> RoleLimits {
        role_limits(&member.view(&self.roles_of(member)), &self.server())
    }

    pub fn effective_limits(&self, member: &Member, channel: Option<&Channel>) -> RoleLimits {
        let roles = self.roles_of(member);
        let channel = channel.map(Channel::view);
        effective_limits(&member.view(&roles), &self.server(), channel.as_ref())
    }

    pub fn ceiling(&self, member: &Member) -> RoleLimits {
        limit_grant_ceiling(&member.view(&self.roles_of(member)), &self.server())
    }
}

/// The configuration of a group of property tests: at least `cases` cases
/// (the `PROPTEST_CASES` variable can ask for more), and failing inputs
/// saved to `regressions`, a path relative to the crate, so that they can
/// be checked in and are tried first on every later run.
pub fn config(cases: u32, regressions: &'static str) -> ProptestConfig {
    let defaults = ProptestConfig::default();
    ProptestConfig {
        cases: defaults.cases.max(cases),
        failure_persistence: Some(Box::new(FileFailurePersistence::Direct(regressions))),
        ..defaults
    }
}

/// The bits that resolution treats specially, plus the ones the hierarchy
/// tests need often. Repeated entries are drawn more often.
const FAVOURED: &[Permissions] = &[
    Permissions::VIEW_CHANNEL,
    Permissions::VIEW_CHANNEL,
    Permissions::MANAGE_ROLES,
    Permissions::MANAGE_ROLES,
    Permissions::MANAGE_CHANNELS,
    Permissions::KICK_MEMBERS,
    Permissions::CHANGE_NICKNAME,
    Permissions::SEND_MESSAGES,
    Permissions::SEND_MESSAGES,
    Permissions::READ_MESSAGE_HISTORY,
    Permissions::ATTACH_FILES,
    Permissions::EMBED_LINKS,
    Permissions::MENTION_EVERYONE,
    Permissions::CONNECT,
    Permissions::CONNECT,
    Permissions::SPEAK,
    Permissions::STREAM_SCREEN,
    Permissions::MUTE_MEMBERS,
    Permissions::MOVE_MEMBERS,
];

/// One bit: usually a favoured one, sometimes any of bits 0 to 62.
pub fn single_bit() -> impl Strategy<Value = Permissions> {
    prop_oneof![
        3 => proptest::sample::select(FAVOURED),
        1 => (0u32..63).prop_map(|bit| Permissions::from_bits_truncate(1u64 << bit)),
    ]
}

/// A sparse mask of up to seven bits. `Administrator` is rare.
pub fn permissions() -> impl Strategy<Value = Permissions> {
    proptest::collection::vec(single_bit(), 0..8).prop_map(|bits| bits.into_iter().collect())
}

/// Any mask at all, about half of the bits set.
pub fn dense_permissions() -> impl Strategy<Value = Permissions> {
    any::<u64>().prop_map(Permissions::from_bits_truncate)
}

/// The mask of a role: sparse, and an administrator role one time in ten.
pub fn role_permissions() -> impl Strategy<Value = Permissions> {
    prop_oneof![
        9 => permissions(),
        1 => permissions().prop_map(|mask| mask | Permissions::ADMINISTRATOR),
    ]
}

fn limit_value() -> impl Strategy<Value = u32> {
    prop_oneof![Just(0), 0u32..6, Just(50), Just(8000), Just(u32::MAX)]
}

/// Limits drawn from a few values, so that equal and adjacent values are
/// common.
pub fn limits() -> impl Strategy<Value = RoleLimits> {
    proptest::array::uniform7(limit_value()).prop_map(|values| {
        let mut limits = RoleLimits::NONE;
        for (kind, value) in LimitKind::ALL.into_iter().zip(values) {
            limits.set(kind, value);
        }
        limits
    })
}

type RawMember = (u8, u8, u8, Option<u8>);
type RawChannel = (Vec<(u8, Permissions, Permissions)>, RoleLimits);

/// A small server: up to five roles, two to five members (the first is the
/// owner) and up to three channels with overwrites. Member state covers
/// active and ended timeouts, young and old members and speak grants.
pub fn world() -> impl Strategy<Value = World> {
    let everyone = prop_oneof![
        3 => permissions().prop_map(|toggled| EVERYONE_PERMISSIONS ^ toggled),
        1 => permissions(),
    ];
    let roles = proptest::collection::vec((role_permissions(), limits()), 0..=5);
    let members = proptest::collection::vec(
        (any::<u8>(), 0u8..3, 0u8..6, proptest::option::of(0u8..3)),
        2..=5,
    );
    let channel = (
        proptest::collection::vec((any::<u8>(), permissions(), permissions()), 0..6),
        prop_oneof![3 => Just(RoleLimits::UNLIMITED), 1 => limits()],
    );
    let channels = proptest::collection::vec(channel, 0..=3);
    let caps = prop_oneof![1 => Just(RoleLimits::UNLIMITED), 2 => limits()];
    let policy = (
        prop_oneof![Just(0u64), Just(5), Just(100)],
        any::<bool>(),
        permissions(),
    );
    (everyone, limits(), roles, members, channels, caps, policy).prop_map(
        |(everyone, everyone_limits, roles, members, channels, caps, policy)| {
            build_world(
                everyone,
                everyone_limits,
                &roles,
                &members,
                &channels,
                caps,
                policy,
            )
        },
    )
}

fn build_world(
    everyone: Permissions,
    everyone_limits: RoleLimits,
    raw_roles: &[(Permissions, RoleLimits)],
    raw_members: &[RawMember],
    raw_channels: &[RawChannel],
    caps: RoleLimits,
    (period_ms, require_role, restricted): (u64, bool, Permissions),
) -> World {
    let now = NOW;
    let roles: Vec<Role> = raw_roles
        .iter()
        .zip(1u32..)
        .map(|(&(permissions, limits), position)| Role {
            id: RoleId(EVERYONE.0 + u64::from(position)),
            position,
            permissions,
            limits,
        })
        .collect();
    let members: Vec<Member> = raw_members
        .iter()
        .zip(1u64..)
        .map(|(&(role_mask, joined, timeout, grant), id)| Member {
            id: UserId(id),
            roles: roles
                .iter()
                .enumerate()
                .filter(|(index, _)| role_mask & (1 << index) != 0)
                .map(|(_, role)| role.id)
                .collect(),
            joined_at: match joined {
                0 => Timestamp(0),
                1 => Timestamp(now.0 - 3),
                _ => Timestamp(now.0 + 7),
            },
            timeout_until: match timeout {
                0..=2 => None,
                3 => Some(now),
                4 => Some(Timestamp(now.0 + 1)),
                _ => Some(Timestamp(now.0 + 1000)),
            },
            speak_grant: grant.map(|index| ChannelId(500 + u64::from(index))),
        })
        .collect();
    let mut targets = vec![OverwriteTarget::Role(EVERYONE)];
    targets.extend(roles.iter().map(|role| OverwriteTarget::Role(role.id)));
    targets.extend(
        members
            .iter()
            .map(|member| OverwriteTarget::Member(member.id)),
    );
    let channels = raw_channels
        .iter()
        .zip(500u64..)
        .map(|((raw_overwrites, caps), id)| {
            let mut overwrites: Vec<Overwrite> = Vec::new();
            for &(selector, allow, deny) in raw_overwrites {
                let target = targets[usize::from(selector) % targets.len()];
                let overwrite = Overwrite::new(target, allow, deny - allow)
                    .expect("deny was made disjoint from allow");
                let taken = overwrites
                    .iter()
                    .any(|existing| existing.target() == target);
                if !taken && !overwrite.is_empty() {
                    overwrites.push(overwrite);
                }
            }
            Channel {
                id: ChannelId(id),
                overwrites,
                caps: *caps,
            }
        })
        .collect();
    World {
        owner: OWNER,
        everyone: Role {
            id: EVERYONE,
            position: 0,
            permissions: everyone,
            limits: everyone_limits,
        },
        roles,
        members,
        channels,
        caps,
        policy: NewMemberPolicy {
            period_ms,
            require_role,
            restricted,
        },
        now,
    }
}
