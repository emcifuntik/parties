//! The model types that resolution and the hierarchy rules read
//! (`docs/rust-rewrite/05-permissions-and-roles.md` section 2).
//!
//! The `*View` types borrow from whatever the caller stores: they hold
//! slices and references, so building one costs no allocation.

use core::fmt;

use crate::ids::{ChannelId, RoleId, UserId};
use crate::limits::RoleLimits;
use crate::permissions::Permissions;

/// A point in time: milliseconds since the Unix epoch.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Timestamp(pub u64);

/// A role as the permission engine sees it. Name, colour and display flags
/// are not part of any rule and stay with the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Role {
    /// The role's identifier.
    pub id: RoleId,
    /// Unique among the roles of a server; higher is more senior.
    /// `@everyone` has position 0 and nothing else may.
    pub position: u32,
    /// The permissions the role grants server-wide.
    pub permissions: Permissions,
    /// The numeric limits the role grants. See [`RoleLimits`].
    pub limits: RoleLimits,
}

/// Who a channel overwrite applies to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum OverwriteTarget {
    /// Every member that has this role. The `@everyone` role is a valid
    /// target and is the least specific one.
    Role(RoleId),
    /// One member. The most specific target.
    Member(UserId),
}

/// A channel overwrite: for one target, the bits it allows and the bits it
/// denies in that channel. The two masks never overlap.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Overwrite {
    target: OverwriteTarget,
    allow: Permissions,
    deny: Permissions,
}

/// Returned by [`Overwrite::new`] when a bit is both allowed and denied.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OverlapError {
    /// The bits found in both masks.
    pub overlap: Permissions,
}

impl fmt::Display for OverlapError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "an overwrite cannot both allow and deny: {}",
            self.overlap
        )
    }
}

impl core::error::Error for OverlapError {}

impl Overwrite {
    /// Builds an overwrite. Fails when `allow` and `deny` share a bit, which
    /// the storage schema forbids as well.
    pub const fn new(
        target: OverwriteTarget,
        allow: Permissions,
        deny: Permissions,
    ) -> Result<Self, OverlapError> {
        let overlap = allow.intersection(deny);
        if overlap.is_empty() {
            Ok(Self {
                target,
                allow,
                deny,
            })
        } else {
            Err(OverlapError { overlap })
        }
    }

    /// Who the overwrite applies to.
    #[must_use]
    pub const fn target(&self) -> OverwriteTarget {
        self.target
    }

    /// The bits the overwrite allows.
    #[must_use]
    pub const fn allow(&self) -> Permissions {
        self.allow
    }

    /// The bits the overwrite denies.
    #[must_use]
    pub const fn deny(&self) -> Permissions {
        self.deny
    }

    /// Whether the overwrite neither allows nor denies anything.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.allow.is_empty() && self.deny.is_empty()
    }
}

/// The combined `(allow, deny)` masks that `overwrites` hold for `target`.
/// Both are empty when there is no overwrite for it. Storage allows at most
/// one overwrite per target; if a slice holds several, they are united.
#[must_use]
pub fn masks_for(overwrites: &[Overwrite], target: OverwriteTarget) -> (Permissions, Permissions) {
    overwrites
        .iter()
        .filter(|overwrite| overwrite.target == target)
        .fold(
            (Permissions::EMPTY, Permissions::EMPTY),
            |(allow, deny), overwrite| (allow | overwrite.allow, deny | overwrite.deny),
        )
}

/// Whether a channel's overwrites equal its category's, which is what the
/// client shows as "synced". Order does not matter and an overwrite with two
/// empty masks counts as absent.
#[must_use]
pub fn overwrites_synced(channel: &[Overwrite], category: &[Overwrite]) -> bool {
    let same = |overwrite: &Overwrite| {
        masks_for(channel, overwrite.target) == masks_for(category, overwrite.target)
    };
    channel.iter().all(same) && category.iter().all(same)
}

/// A channel as the permission engine sees it. Categories are channels too:
/// they carry overwrites, and resolution only ever reads the overwrites of
/// the channel it is asked about.
#[derive(Clone, Copy, Debug)]
pub struct ChannelView<'a> {
    /// The channel's identifier.
    pub id: ChannelId,
    /// The channel's own overwrites, in any order.
    pub overwrites: &'a [Overwrite],
    /// The channel's own caps on numeric limits, for example a channel
    /// bitrate. [`RoleLimits::UNLIMITED`] when the channel has none.
    pub limit_caps: RoleLimits,
}

impl<'a> ChannelView<'a> {
    /// A channel with the given overwrites and no caps of its own.
    #[must_use]
    pub const fn new(id: ChannelId, overwrites: &'a [Overwrite]) -> Self {
        Self {
            id,
            overwrites,
            limit_caps: RoleLimits::UNLIMITED,
        }
    }

    /// The combined `(allow, deny)` masks of the channel for `target`. See
    /// [`masks_for`].
    #[must_use]
    pub fn masks_for(&self, target: OverwriteTarget) -> (Permissions, Permissions) {
        masks_for(self.overwrites, target)
    }
}

/// A member as the permission engine sees them.
#[derive(Clone, Copy, Debug)]
pub struct MemberView<'a> {
    /// The member's user identifier.
    pub user_id: UserId,
    /// The roles the member has, in any order. `@everyone` is implied and
    /// does not need to be listed; listing it changes nothing.
    pub roles: &'a [&'a Role],
    /// When the member joined. Decides whether the new-member restrictions
    /// still apply.
    pub joined_at: Timestamp,
    /// The member is timed out until this instant, exclusive.
    pub timeout_until: Option<Timestamp>,
    /// The channel in which a moderator let the member speak after a raised
    /// hand. The caller clears it when the member leaves that channel.
    pub speak_grant: Option<ChannelId>,
}

impl<'a> MemberView<'a> {
    /// A member with these roles who joined at time zero and has no
    /// moderation state.
    #[must_use]
    pub const fn new(user_id: UserId, roles: &'a [&'a Role]) -> Self {
        Self {
            user_id,
            roles,
            joined_at: Timestamp(0),
            timeout_until: None,
            speak_grant: None,
        }
    }

    /// Whether the member's role list names this role.
    #[must_use]
    pub fn has_role(&self, role: RoleId) -> bool {
        self.roles.iter().any(|held| held.id == role)
    }

    /// Whether the member is timed out at `now`. A timeout until T has ended
    /// at T.
    #[must_use]
    pub fn is_timed_out(&self, now: Timestamp) -> bool {
        self.timeout_until.is_some_and(|until| now < until)
    }
}

/// The server setting that restricts members who are not yet trusted
/// ("restricted until verified").
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NewMemberPolicy {
    /// The new-member period in milliseconds. A member who joined less than
    /// this long ago is restricted. 0 turns the age rule off.
    pub period_ms: u64,
    /// When set, a member with no role besides `@everyone` is restricted,
    /// whatever their age.
    pub require_role: bool,
    /// The bits removed from a restricted member.
    pub restricted: Permissions,
}

impl NewMemberPolicy {
    /// Nobody is restricted.
    pub const OFF: Self = Self {
        period_ms: 0,
        require_role: false,
        restricted: Permissions::EMPTY,
    };

    /// Whether the policy restricts `member` at `now`. `everyone` is the id
    /// of the `@everyone` role, which does not count as a role here.
    ///
    /// A member whose join time lies in the future (a clock that moved
    /// back) counts as having just joined.
    #[must_use]
    pub fn applies_to(&self, member: &MemberView<'_>, everyone: RoleId, now: Timestamp) -> bool {
        let age_ms = now.0.saturating_sub(member.joined_at.0);
        let too_young = age_ms < self.period_ms;
        let without_role =
            self.require_role && !member.roles.iter().any(|role| role.id != everyone);
        too_young || without_role
    }
}

/// The server as the permission engine sees it.
#[derive(Clone, Copy, Debug)]
pub struct ServerView<'a> {
    /// The owner. Set on the server host and never changed by a request.
    pub owner_id: UserId,
    /// The built-in role every member has.
    pub everyone: &'a Role,
    /// The server-wide caps on numeric limits.
    pub limit_caps: RoleLimits,
    /// The restrictions on members who are not yet trusted.
    pub new_members: NewMemberPolicy,
}

impl<'a> ServerView<'a> {
    /// A server with no caps and no new-member restrictions.
    #[must_use]
    pub const fn new(owner_id: UserId, everyone: &'a Role) -> Self {
        Self {
            owner_id,
            everyone,
            limit_caps: RoleLimits::UNLIMITED,
            new_members: NewMemberPolicy::OFF,
        }
    }

    /// Whether `user` is the owner.
    #[must_use]
    pub fn is_owner(&self, user: UserId) -> bool {
        user == self.owner_id
    }

    /// Whether `role` is the `@everyone` role.
    #[must_use]
    pub fn is_everyone(&self, role: &Role) -> bool {
        role.id == self.everyone.id
    }

    /// The position the hierarchy rules use for `role`: 0 for `@everyone`,
    /// the role's own position otherwise.
    #[must_use]
    pub fn position_of(&self, role: &Role) -> u32 {
        if self.is_everyone(role) {
            0
        } else {
            role.position
        }
    }

    /// `top(m)`: the highest position among the member's roles, 0 with only
    /// `@everyone`. Among members other than the owner, only this decides
    /// who outranks whom; the owner outranks everyone whatever their roles.
    #[must_use]
    pub fn top_position(&self, member: &MemberView<'_>) -> u32 {
        member
            .roles
            .iter()
            .map(|role| self.position_of(role))
            .max()
            .unwrap_or(0)
    }
}
