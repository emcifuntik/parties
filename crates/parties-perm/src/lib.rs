//! The permission engine of Parties 2.0.
//!
//! One implementation of `docs/rust-rewrite/05-permissions-and-roles.md`,
//! used by the server to enforce, by the client to preview, and by
//! `partiesctl`. Everything here is a pure function of its arguments: no
//! I/O, no clock, no allocation, no `unsafe`. Time is always passed in.
//!
//! | Module | Section of the document | What it holds |
//! |---|---|---|
//! | [`permissions`] | 3 | The bit set, the catalogue with names, groups and scopes |
//! | [`model`] | 2 | Roles, overwrites, and the views of member, channel and server |
//! | [`resolve`] | 4, 8 | [`effective`] permissions, step by step |
//! | [`hierarchy`] | 5 | Rules H1 to H10 as `check_*` functions |
//! | [`escalation`] | 5 (H4) | "No escalation" on masks and limits |
//! | [`limits`] | 6 | Numeric role limits and how they combine |
//! | [`gate`] | 7.1 | The request-independent parts of the request gate |
//! | [`defaults`] | 9 | The roles and settings of a new server |
//! | [`ids`] | | Identifier newtypes, until `parties-proto` provides them |
//!
//! A check returns `Result<(), Denied>`; [`Denied`] says why it refused and
//! [`Denied::rule`] which hierarchy rule did.
//!
//! # Example
//!
//! ```
//! use parties_perm::{
//!     ChannelId, ChannelView, Denied, MemberView, Overwrite, OverwriteTarget, Permissions,
//!     Preset, RoleId, ServerDefaults, ServerView, Timestamp, UserId, check_moderate, effective,
//!     require,
//! };
//!
//! // The roles of a new server. The server assigns the identifiers.
//! let defaults = ServerDefaults::new(Preset::Friends);
//! let everyone = defaults.everyone.to_role(RoleId(1));
//! let moderator_role = defaults.moderator.to_role(RoleId(2));
//! let server = ServerView::new(UserId(100), &everyone);
//! let now = Timestamp(1_700_000_000_000);
//!
//! let held = [&moderator_role];
//! let moderator = MemberView::new(UserId(7), &held);
//! let member = MemberView::new(UserId(8), &[]);
//!
//! // A request passes the permission test first, then the hierarchy test.
//! let kick = |actor: &MemberView<'_>, target: &MemberView<'_>| {
//!     require(effective(actor, &server, None, now), Permissions::KICK_MEMBERS)?;
//!     check_moderate(actor, target, &server)
//! };
//! assert_eq!(kick(&moderator, &member), Ok(()));
//! assert_eq!(kick(&moderator, &moderator), Err(Denied::EqualRank));
//! assert_eq!(
//!     kick(&member, &moderator),
//!     Err(Denied::MissingPermissions(Permissions::KICK_MEMBERS))
//! );
//!
//! // A moderated channel: `@everyone` may join and listen, but not speak.
//! let everyone_here = OverwriteTarget::Role(everyone.id);
//! let overwrites = [Overwrite::new(everyone_here, Permissions::EMPTY, Permissions::SPEAK)?];
//! let stage = ChannelView::new(ChannelId(50), &overwrites);
//! let in_stage = effective(&member, &server, Some(&stage), now);
//! assert!(in_stage.contains(Permissions::CONNECT));
//! assert!(!in_stage.contains(Permissions::SPEAK));
//! # Ok::<(), parties_perm::OverlapError>(())
//! ```

#![cfg_attr(not(test), no_std)]
#![warn(missing_docs)]

pub mod defaults;
pub mod denied;
pub mod escalation;
pub mod gate;
pub mod hierarchy;
pub mod ids;
pub mod limits;
pub mod model;
pub mod permissions;
pub mod resolve;

pub use defaults::{Preset, RoleTemplate, ServerDefaults};
pub use denied::{Denied, Rule};
pub use escalation::{check_limit_change, check_permission_change};
pub use gate::{Hierarchy, HierarchyTarget, Scope, check_hierarchy, require};
pub use hierarchy::{
    OverwriteSubject, check_assign_role, check_create_role, check_delete_overwrite,
    check_delete_role, check_edit_role, check_leave, check_moderate, check_move_role,
    check_revoke_role, check_role_below, check_set_overwrite,
};
pub use ids::{ChannelId, MessageId, RoleId, UserId};
pub use limits::{LimitKind, RoleLimits, effective_limits, limit_grant_ceiling, role_limits};
pub use model::{
    ChannelView, MemberView, NewMemberPolicy, OverlapError, Overwrite, OverwriteTarget, Role,
    ServerView, Timestamp, masks_for, overwrites_synced,
};
pub use permissions::{
    CATALOGUE, PermissionGroup, PermissionInfo, PermissionScope, Permissions, PermissionsIter,
};
pub use resolve::{
    apply_implications, apply_overwrites, base_permissions, can_view, effective, is_administrator,
};
