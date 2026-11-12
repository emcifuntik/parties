//! The typed reason a check refuses with.

use core::fmt;

use crate::limits::LimitKind;
use crate::permissions::Permissions;

/// A hierarchy rule of `docs/rust-rewrite/05-permissions-and-roles.md`
/// section 5 that can refuse a request.
///
/// H9 is not listed: it states that administrators are still bound by H2, H3
/// and H5, so a refusal of an administrator carries one of those rules (or
/// H7).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Rule {
    /// The owner cannot be the target of any moderation action.
    H1,
    /// A role can be created, edited, deleted or reordered only strictly
    /// below the actor's top position.
    H2,
    /// A role can be assigned or revoked only strictly below the actor's top
    /// position, and only to or from a member below the actor.
    H3,
    /// No escalation: nobody grants a bit or a limit they do not hold.
    H4,
    /// Moderation needs the target strictly below the actor.
    H5,
    /// Editing overwrites needs `ManageRoles` in the channel and a target
    /// below the actor.
    H6,
    /// Equal top positions: neither member can act on the other.
    H7,
    /// `@everyone` cannot be deleted, moved or assigned.
    H8,
    /// The owner cannot leave the server or be removed.
    H10,
}

impl Rule {
    /// The rule's name in the design document, for example `"H4"`.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::H1 => "H1",
            Self::H2 => "H2",
            Self::H3 => "H3",
            Self::H4 => "H4",
            Self::H5 => "H5",
            Self::H6 => "H6",
            Self::H7 => "H7",
            Self::H8 => "H8",
            Self::H10 => "H10",
        }
    }
}

/// Why a check refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Denied {
    /// The actor lacks these required permissions in the scope of the
    /// request. Returned by [`crate::require`]; not a hierarchy rule.
    MissingPermissions(Permissions),
    /// H1: the target is the owner.
    OwnerIsTarget,
    /// H2: the role is not strictly below the actor's top position. This
    /// covers the place a role is created at or moved to as well.
    RoleNotBelowActor,
    /// H3: the role to assign or revoke is not strictly below the actor's
    /// top position.
    AssignedRoleNotBelowActor,
    /// H3: the member who would gain or lose the role outranks the actor.
    AssignTargetNotBelowActor,
    /// H4: the change touches these permission bits, which the actor does
    /// not hold.
    PermissionEscalation(Permissions),
    /// H4: the change sets this limit above what the actor may grant.
    LimitEscalation(LimitKind),
    /// H5: the target of a moderation action outranks the actor.
    TargetNotBelowActor,
    /// H6: the actor lacks `ManageRoles` in the channel.
    ManageRolesRequired,
    /// H6: the overwrite is for a role at or above the actor's top position,
    /// or for a member who outranks the actor.
    OverwriteTargetNotBelowActor,
    /// H7: actor and target have equal top positions. Acting on oneself
    /// where a rule needs the target strictly below is refused with this
    /// too.
    EqualRank,
    /// H8: `@everyone` cannot be deleted, moved, assigned or revoked, and
    /// no other role can take position 0.
    EveryoneRole,
    /// H10: the owner cannot leave the server.
    OwnerCannotLeave,
    /// The change adds these server-scope bits to an overwrite. They cannot
    /// be allowed or denied per channel.
    ServerScopeInOverwrite(Permissions),
    /// The overwrite would both allow and deny these bits.
    OverlappingOverwrite(Permissions),
}

impl Denied {
    /// The hierarchy rule that refused, or `None` when the refusal is a
    /// missing permission or an invalid overwrite.
    #[must_use]
    pub const fn rule(&self) -> Option<Rule> {
        match self {
            Self::OwnerIsTarget => Some(Rule::H1),
            Self::RoleNotBelowActor => Some(Rule::H2),
            Self::AssignedRoleNotBelowActor | Self::AssignTargetNotBelowActor => Some(Rule::H3),
            Self::PermissionEscalation(_) | Self::LimitEscalation(_) => Some(Rule::H4),
            Self::TargetNotBelowActor => Some(Rule::H5),
            Self::ManageRolesRequired | Self::OverwriteTargetNotBelowActor => Some(Rule::H6),
            Self::EqualRank => Some(Rule::H7),
            Self::EveryoneRole => Some(Rule::H8),
            Self::OwnerCannotLeave => Some(Rule::H10),
            Self::MissingPermissions(_)
            | Self::ServerScopeInOverwrite(_)
            | Self::OverlappingOverwrite(_) => None,
        }
    }
}

impl fmt::Display for Denied {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::MissingPermissions(missing) => write!(f, "missing permission: {missing}"),
            Self::OwnerIsTarget => f.write_str("the owner cannot be the target of this action"),
            Self::RoleNotBelowActor => {
                f.write_str("the role is not below the highest role of the actor")
            }
            Self::AssignedRoleNotBelowActor => {
                f.write_str("only roles below the highest role of the actor can be assigned")
            }
            Self::AssignTargetNotBelowActor => {
                f.write_str("the member whose roles would change outranks the actor")
            }
            Self::PermissionEscalation(bits) => {
                write!(f, "the actor does not hold these permissions: {bits}")
            }
            Self::LimitEscalation(kind) => {
                write!(
                    f,
                    "the limit {} is above what the actor may grant",
                    kind.name()
                )
            }
            Self::TargetNotBelowActor => f.write_str("the target outranks the actor"),
            Self::ManageRolesRequired => {
                f.write_str("editing overwrites needs ManageRoles in the channel")
            }
            Self::OverwriteTargetNotBelowActor => {
                f.write_str("the target of the overwrite is not below the actor")
            }
            Self::EqualRank => f.write_str("actor and target have the same rank"),
            Self::EveryoneRole => {
                f.write_str("the @everyone role cannot be deleted, moved or assigned")
            }
            Self::OwnerCannotLeave => f.write_str("the owner cannot leave the server"),
            Self::ServerScopeInOverwrite(bits) => {
                write!(f, "these permissions cannot be set per channel: {bits}")
            }
            Self::OverlappingOverwrite(bits) => {
                write!(f, "an overwrite cannot both allow and deny: {bits}")
            }
        }
    }
}

impl core::error::Error for Denied {}
