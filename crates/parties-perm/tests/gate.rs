//! The request-independent parts of the request gate (section 7.1) and the
//! two primitives of rule H4.

mod common;

use common::{EVERYONE, OWNER, role};
use parties_perm::{
    ChannelId, Denied, Hierarchy, HierarchyTarget, LimitKind, MemberView, MessageId, Permissions,
    Role, RoleId, RoleLimits, Scope, ServerView, UserId, check_hierarchy, check_limit_change,
    check_permission_change, require,
};

const VIEW: Permissions = Permissions::VIEW_CHANNEL;
const CONNECT: Permissions = Permissions::CONNECT;
const KICK: Permissions = Permissions::KICK_MEMBERS;
const SPEAK: Permissions = Permissions::SPEAK;
const NONE: Permissions = Permissions::EMPTY;

/// A row of the mask table: name, held, old, new, expected.
type MaskCase = (
    &'static str,
    Permissions,
    Permissions,
    Permissions,
    Result<(), Denied>,
);

/// A row of the limit table: name, ceiling, old, new, expected.
type LimitCase = (&'static str, u32, u32, u32, Result<(), Denied>);

#[test]
fn require_names_the_missing_bits() {
    let cases: &[(&str, Permissions, Permissions, Result<(), Denied>)] = &[
        ("nothing required", NONE, NONE, Ok(())),
        ("nothing required, something held", VIEW, NONE, Ok(())),
        ("one bit, held", VIEW | KICK, KICK, Ok(())),
        (
            "one bit, missing",
            VIEW,
            KICK,
            Err(Denied::MissingPermissions(KICK)),
        ),
        (
            "two bits, both held",
            VIEW | CONNECT | KICK,
            VIEW | CONNECT,
            Ok(()),
        ),
        (
            "two bits, one missing",
            VIEW,
            VIEW | CONNECT,
            Err(Denied::MissingPermissions(CONNECT)),
        ),
        (
            "two bits, both missing",
            KICK,
            VIEW | CONNECT,
            Err(Denied::MissingPermissions(VIEW | CONNECT)),
        ),
        (
            "nothing held",
            NONE,
            KICK,
            Err(Denied::MissingPermissions(KICK)),
        ),
        (
            "everything held",
            Permissions::ALL,
            Permissions::ALL,
            Ok(()),
        ),
        (
            "all of the catalogue but one",
            Permissions::KNOWN - SPEAK,
            Permissions::KNOWN,
            Err(Denied::MissingPermissions(SPEAK)),
        ),
    ];
    for &(name, held, all_of, expected) in cases {
        assert_eq!(require(held, all_of), expected, "{name}");
    }
    assert_eq!(Denied::MissingPermissions(KICK).rule(), None);
}

#[test]
fn the_hierarchy_test_dispatches_on_the_target() {
    let everyone = role(EVERYONE.0, 0, NONE);
    let low = role(101, 1, NONE);
    let mid = role(102, 2, NONE);
    let high = role(103, 3, NONE);
    let server = ServerView::new(OWNER, &everyone);

    let mid_roles: &[&Role] = &[&mid];
    let low_roles: &[&Role] = &[&low];
    let high_roles: &[&Role] = &[&high];
    let actor = MemberView::new(UserId(2), mid_roles);
    let below = MemberView::new(UserId(3), low_roles);
    let equal = MemberView::new(UserId(4), mid_roles);
    let above = MemberView::new(UserId(5), high_roles);
    let owner = MemberView::new(OWNER, &[]);

    let cases: &[(&str, HierarchyTarget<'_>, Result<(), Denied>)] = &[
        ("no target", HierarchyTarget::None, Ok(())),
        ("member below", HierarchyTarget::Member(&below), Ok(())),
        (
            "member at the same rank",
            HierarchyTarget::Member(&equal),
            Err(Denied::EqualRank),
        ),
        (
            "member above",
            HierarchyTarget::Member(&above),
            Err(Denied::TargetNotBelowActor),
        ),
        (
            "the actor",
            HierarchyTarget::Member(&actor),
            Err(Denied::EqualRank),
        ),
        (
            "the owner",
            HierarchyTarget::Member(&owner),
            Err(Denied::OwnerIsTarget),
        ),
        ("role below", HierarchyTarget::Role(&low), Ok(())),
        ("@everyone", HierarchyTarget::Role(&everyone), Ok(())),
        (
            "the actor's top role",
            HierarchyTarget::Role(&mid),
            Err(Denied::RoleNotBelowActor),
        ),
        (
            "role above",
            HierarchyTarget::Role(&high),
            Err(Denied::RoleNotBelowActor),
        ),
    ];
    for &(name, target, expected) in cases {
        assert_eq!(check_hierarchy(&actor, &server, target), expected, "{name}");
        // The owner passes, except as their own target.
        let for_owner = match target {
            HierarchyTarget::Member(member) if member.user_id == OWNER => {
                Err(Denied::OwnerIsTarget)
            }
            _ => Ok(()),
        };
        assert_eq!(
            check_hierarchy(&owner, &server, target),
            for_owner,
            "{name}, as the owner"
        );
    }
}

#[test]
fn scope_and_hierarchy_are_plain_values() {
    // The server's requirement table stores these; they must compare and
    // copy like the ids they carry.
    let scope = Scope::Channel(ChannelId(7));
    let copy = scope;
    assert_eq!(scope, copy);
    assert_ne!(scope, Scope::Server);
    assert_ne!(scope, Scope::Channel(ChannelId(8)));
    assert_ne!(
        Scope::ChannelOf(MessageId(7)),
        Scope::ChannelOf(MessageId(8))
    );

    let hierarchy = Hierarchy::TargetMember(UserId(3));
    let copy = hierarchy;
    assert_eq!(hierarchy, copy);
    assert_ne!(hierarchy, Hierarchy::None);
    assert_ne!(
        Hierarchy::TargetRole(RoleId(3)),
        Hierarchy::TargetRole(RoleId(4))
    );
}

#[test]
fn a_mask_changes_only_in_held_bits() {
    let a = Permissions::VIEW_CHANNEL;
    let b = Permissions::SEND_MESSAGES;
    let c = Permissions::SPEAK;
    let d = Permissions::ADMINISTRATOR;
    let escalation = |bits| Err(Denied::PermissionEscalation(bits));

    // (name, held, old, new, expected)
    let cases: &[MaskCase] = &[
        ("no change, nothing held", NONE, a | b, a | b, Ok(())),
        ("no change at all", NONE, NONE, NONE, Ok(())),
        ("add a held bit", a, NONE, a, Ok(())),
        ("remove a held bit", a, a, NONE, Ok(())),
        ("add a bit that is not held", a, NONE, b, escalation(b)),
        ("remove a bit that is not held", a, b, NONE, escalation(b)),
        ("add held and not held", a, NONE, a | b, escalation(b)),
        (
            "keep a bit that is not held, add a held one",
            a,
            b,
            a | b,
            Ok(()),
        ),
        (
            "keep a bit that is not held, remove a held one",
            a,
            a | b,
            b,
            Ok(()),
        ),
        ("swap", a, a, b, escalation(b)),
        (
            "swap two bits that are not held",
            a,
            b,
            c,
            escalation(b | c),
        ),
        ("everything held", Permissions::ALL, a, b | c | d, Ok(())),
        (
            "add Administrator without holding it",
            Permissions::ALL - d,
            NONE,
            d,
            escalation(d),
        ),
        (
            "clear a mask the actor holds entirely",
            a | b | c,
            a | b | c,
            NONE,
            Ok(()),
        ),
        (
            "from nothing to everything, nothing held",
            NONE,
            NONE,
            Permissions::ALL,
            escalation(Permissions::ALL),
        ),
    ];
    for &(name, held, old, new, expected) in cases {
        assert_eq!(check_permission_change(held, old, new), expected, "{name}");
        // The rule is symmetric in old and new: undoing a change needs the
        // same bits as making it.
        assert_eq!(
            check_permission_change(held, new, old),
            expected,
            "{name}, reversed"
        );
    }
}

#[test]
fn a_limit_changes_only_up_to_the_ceiling() {
    let kind = LimitKind::MaxUploadSize;
    let other = LimitKind::MaxVideoFps;
    let limits = |value| RoleLimits::NONE.with(kind, value);
    let escalation = Err(Denied::LimitEscalation(kind));

    // (name, ceiling, old, new, expected)
    let cases: &[LimitCase] = &[
        ("unchanged, within", 50, 10, 10, Ok(())),
        ("unchanged, above the ceiling", 50, 900, 900, Ok(())),
        ("raise to below the ceiling", 50, 10, 49, Ok(())),
        ("raise to the ceiling", 50, 10, 50, Ok(())),
        ("raise to one above the ceiling", 50, 10, 51, escalation),
        ("lower within", 50, 40, 0, Ok(())),
        ("lower from above to the ceiling", 50, 900, 50, Ok(())),
        (
            "lower from above to one above the ceiling",
            50,
            900,
            51,
            escalation,
        ),
        ("raise from above", 50, 900, 901, escalation),
        ("a ceiling of zero allows zero", 0, 5, 0, Ok(())),
        ("a ceiling of zero allows nothing else", 0, 0, 1, escalation),
        ("no ceiling", u32::MAX, 0, u32::MAX, Ok(())),
    ];
    for &(name, ceiling, old, new, expected) in cases {
        assert_eq!(
            check_limit_change(&limits(ceiling), &limits(old), &limits(new)),
            expected,
            "{name}"
        );
    }

    // Each limit is measured against its own ceiling.
    let ceiling = RoleLimits::NONE.with(kind, 50).with(other, 30);
    let new = RoleLimits::NONE.with(kind, 30).with(other, 50);
    assert_eq!(
        check_limit_change(&ceiling, &RoleLimits::NONE, &new),
        Err(Denied::LimitEscalation(other))
    );
    // Every kind is checked.
    for kind in LimitKind::ALL {
        let new = RoleLimits::NONE.with(kind, 1);
        assert_eq!(
            check_limit_change(&RoleLimits::NONE, &RoleLimits::NONE, &new),
            Err(Denied::LimitEscalation(kind)),
            "{kind:?}"
        );
        assert_eq!(
            check_limit_change(&new, &RoleLimits::NONE, &new),
            Ok(()),
            "{kind:?}"
        );
    }
}
