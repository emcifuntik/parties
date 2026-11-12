//! The model types (section 2): overwrites, synced categories, positions
//! and identifiers.

mod common;

use common::{EVERYONE, OWNER, member_overwrite, role, role_overwrite};
use parties_perm::{
    ChannelId, ChannelView, MemberView, MessageId, OverlapError, Overwrite, OverwriteTarget,
    Permissions, Role, RoleId, RoleLimits, ServerView, Timestamp, UserId, masks_for,
    overwrites_synced,
};

const VIEW: Permissions = Permissions::VIEW_CHANNEL;
const SEND: Permissions = Permissions::SEND_MESSAGES;
const SPEAK: Permissions = Permissions::SPEAK;
const PIN: Permissions = Permissions::PIN_MESSAGES;
const NONE: Permissions = Permissions::EMPTY;

#[test]
fn an_overwrite_never_allows_and_denies_the_same_bit() {
    let target = OverwriteTarget::Role(RoleId(7));
    let cases: &[(&str, Permissions, Permissions, Option<Permissions>)] = &[
        ("both empty", NONE, NONE, None),
        ("allow only", VIEW | SEND, NONE, None),
        ("deny only", NONE, VIEW | SEND, None),
        ("disjoint", VIEW, SEND | SPEAK, None),
        ("one shared bit", VIEW | SEND, SEND | SPEAK, Some(SEND)),
        ("identical", VIEW | SEND, VIEW | SEND, Some(VIEW | SEND)),
        (
            "everything against one bit",
            Permissions::ALL,
            PIN,
            Some(PIN),
        ),
    ];
    for &(name, allow, deny, overlap) in cases {
        match (Overwrite::new(target, allow, deny), overlap) {
            (Ok(overwrite), None) => {
                assert_eq!(overwrite.target(), target, "{name}");
                assert_eq!(overwrite.allow(), allow, "{name}");
                assert_eq!(overwrite.deny(), deny, "{name}");
                assert_eq!(
                    overwrite.is_empty(),
                    allow.is_empty() && deny.is_empty(),
                    "{name}"
                );
            }
            (Err(error), Some(overlap)) => assert_eq!(error, OverlapError { overlap }, "{name}"),
            (result, _) => panic!("{name}: unexpected {result:?}"),
        }
    }
    let error = Overwrite::new(target, SEND, SEND).unwrap_err();
    assert_eq!(
        error.to_string(),
        "an overwrite cannot both allow and deny: SendMessages"
    );
}

#[test]
fn masks_for_finds_the_target() {
    let role = RoleId(7);
    let user = UserId(7); // the same number as the role: the kind decides
    let overwrites = [
        role_overwrite(role, VIEW, SEND),
        member_overwrite(user, SPEAK, PIN),
        role_overwrite(EVERYONE, NONE, VIEW),
    ];
    assert_eq!(
        masks_for(&overwrites, OverwriteTarget::Role(role)),
        (VIEW, SEND)
    );
    assert_eq!(
        masks_for(&overwrites, OverwriteTarget::Member(user)),
        (SPEAK, PIN)
    );
    assert_eq!(
        masks_for(&overwrites, OverwriteTarget::Role(EVERYONE)),
        (NONE, VIEW)
    );
    assert_eq!(
        masks_for(&overwrites, OverwriteTarget::Role(RoleId(8))),
        (NONE, NONE)
    );
    assert_eq!(masks_for(&[], OverwriteTarget::Member(user)), (NONE, NONE));

    let channel = ChannelView::new(ChannelId(1), &overwrites);
    assert_eq!(channel.masks_for(OverwriteTarget::Role(role)), (VIEW, SEND));
    assert_eq!(channel.limit_caps, RoleLimits::UNLIMITED);
}

#[test]
fn a_channel_is_synced_when_its_overwrites_equal_the_categorys() {
    let a = role_overwrite(RoleId(7), VIEW, SEND);
    let a_other_masks = role_overwrite(RoleId(7), VIEW, SPEAK);
    let a_swapped = role_overwrite(RoleId(7), SEND, VIEW);
    let b = member_overwrite(UserId(7), SPEAK, NONE);
    let b_as_role = role_overwrite(RoleId(8), SPEAK, NONE);
    let empty = role_overwrite(RoleId(9), NONE, NONE);

    let cases: &[(&str, &[Overwrite], &[Overwrite], bool)] = &[
        ("both without overwrites", &[], &[], true),
        ("identical", &[a, b], &[a, b], true),
        ("order does not matter", &[a, b], &[b, a], true),
        (
            "an empty overwrite counts as absent",
            &[a, empty],
            &[a],
            true,
        ),
        ("only empty overwrites", &[empty], &[], true),
        ("the channel has one more", &[a, b], &[a], false),
        ("the category has one more", &[a], &[a, b], false),
        (
            "the same target with other masks",
            &[a],
            &[a_other_masks],
            false,
        ),
        ("allow and deny swapped", &[a], &[a_swapped], false),
        (
            "a role and a member are different targets",
            &[b],
            &[b_as_role],
            false,
        ),
        ("nothing against something", &[], &[a], false),
    ];
    for &(name, channel, category, synced) in cases {
        assert_eq!(overwrites_synced(channel, category), synced, "{name}");
        assert_eq!(
            overwrites_synced(category, channel),
            synced,
            "{name}, the other way round"
        );
    }
}

#[test]
fn positions_and_top_position() {
    let everyone = role(EVERYONE.0, 0, NONE);
    let low = role(101, 1, NONE);
    let mid = role(102, 7, NONE);
    let high = role(103, 40, NONE);
    let server = ServerView::new(OWNER, &everyone);

    assert!(server.is_owner(OWNER));
    assert!(!server.is_owner(UserId(2)));
    assert!(server.is_everyone(&everyone));
    assert!(!server.is_everyone(&low));
    assert_eq!(server.position_of(&everyone), 0);
    assert_eq!(server.position_of(&mid), 7);
    // @everyone is at 0 whatever the struct says.
    let misplaced = Role {
        position: 99,
        ..everyone
    };
    assert_eq!(server.position_of(&misplaced), 0);

    let cases: &[(&str, &[&Role], u32)] = &[
        ("no roles", &[], 0),
        ("only @everyone", &[&everyone], 0),
        ("one role", &[&mid], 7),
        ("the highest of several", &[&low, &high, &mid], 40),
        ("order does not matter", &[&high, &low], 40),
        (
            "a misplaced @everyone does not count",
            &[&misplaced, &low],
            1,
        ),
    ];
    for &(name, held, top) in cases {
        let member = MemberView::new(UserId(2), held);
        assert_eq!(server.top_position(&member), top, "{name}");
    }
}

#[test]
fn member_view_defaults_and_role_lookup() {
    let low = role(101, 1, NONE);
    let held: &[&Role] = &[&low];
    let member = MemberView::new(UserId(2), held);

    assert_eq!(member.user_id, UserId(2));
    assert_eq!(member.joined_at, Timestamp(0));
    assert_eq!(member.timeout_until, None);
    assert_eq!(member.speak_grant, None);
    assert!(member.has_role(RoleId(101)));
    assert!(!member.has_role(RoleId(102)));
    assert!(!member.has_role(EVERYONE));
    assert!(!member.is_timed_out(Timestamp(u64::MAX)));
}

#[test]
fn identifiers_are_distinct_types_over_u64() {
    assert_eq!(UserId::from(5).get(), 5);
    assert_eq!(u64::from(RoleId(6)), 6);
    assert_eq!(ChannelId(7).0, 7);
    assert_eq!(MessageId::from(8), MessageId(8));
    assert!(RoleId(1) < RoleId(2));
    assert_eq!(UserId(u64::MAX).get(), u64::MAX);
    assert!(Timestamp(1) < Timestamp(2));
    assert_eq!(Timestamp::default(), Timestamp(0));
}
