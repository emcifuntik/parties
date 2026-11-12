//! Rule H4, "no escalation", on masks and on limits.
//!
//! These two functions are the whole rule. The checks in
//! [`crate::hierarchy`] call them with the right inputs; the server's
//! request gate can call them directly for a request that carries masks.

use crate::denied::Denied;
use crate::limits::{LimitKind, RoleLimits};
use crate::permissions::Permissions;

/// H4 for a permission mask: every bit that differs between `old` and `new`
/// must be in `held`.
///
/// `held` is what the actor holds in the scope of the change: server-wide
/// for the mask of a role, in the channel for the masks of an overwrite.
/// The rule covers removing a bit as well as adding one, so a bit the actor
/// does not hold stays exactly as it is. That is what the role editor shows
/// as "locked", and for the deny mask of an overwrite it is what stops a
/// member from handing out a permission by lifting a denial.
pub const fn check_permission_change(
    held: Permissions,
    old: Permissions,
    new: Permissions,
) -> Result<(), Denied> {
    let offending = old.symmetric_difference(new).difference(held);
    if offending.is_empty() {
        Ok(())
    } else {
        Err(Denied::PermissionEscalation(offending))
    }
}

/// H4 for limits: a limit may stay as it is, or change to a value that is at
/// most the same limit in `ceiling`.
///
/// `ceiling` is [`crate::limit_grant_ceiling`] of the actor. The first
/// offending limit, in the order of [`LimitKind::ALL`], is reported.
pub const fn check_limit_change(
    ceiling: &RoleLimits,
    old: &RoleLimits,
    new: &RoleLimits,
) -> Result<(), Denied> {
    let mut i = 0;
    while i < LimitKind::ALL.len() {
        let kind = LimitKind::ALL[i];
        let value = new.get(kind);
        if value != old.get(kind) && value > ceiling.get(kind) {
            return Err(Denied::LimitEscalation(kind));
        }
        i += 1;
    }
    Ok(())
}
