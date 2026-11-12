//! The roles and settings of a new server (section 9).

mod common;

use common::{NOW, OWNER};
use parties_perm::defaults::{
    ADMIN_PERMISSIONS, COMMUNITY_NEW_MEMBER_PERIOD_MS, EVERYONE_NAME, EVERYONE_PERMISSIONS,
    MODERATOR_PERMISSIONS, NEW_MEMBER_RESTRICTED,
};
use parties_perm::{
    Denied, MemberView, NewMemberPolicy, Permissions, Preset, Role, RoleId, RoleLimits,
    ServerDefaults, ServerView, Timestamp, UserId, check_assign_role, check_edit_role,
    check_moderate, effective, effective_limits, require,
};

fn names(list: &[&str]) -> Permissions {
    list.iter()
        .map(|name| Permissions::from_name(name).unwrap_or_else(|| panic!("unknown: {name}")))
        .collect()
}

#[test]
fn default_masks_are_pinned() {
    // The lists are copied from the table in section 9.
    assert_eq!(
        EVERYONE_PERMISSIONS,
        names(&[
            "ViewChannel",
            "CreateJoinCodes",
            "ChangeNickname",
            "SendMessages",
            "ReadMessageHistory",
            "AttachFiles",
            "EmbedLinks",
            "AddReactions",
            "Connect",
            "Speak",
            "UseVoiceActivity",
            "StreamScreen",
            "StreamCamera",
            "UseAuxAudio",
            "RequestToSpeak",
        ])
    );
    assert_eq!(
        MODERATOR_PERMISSIONS,
        names(&[
            "KickMembers",
            "TimeoutMembers",
            "ManageNicknames",
            "ManageMessages",
            "PinMessages",
            "MuteMembers",
            "DeafenMembers",
            "MoveMembers",
            "DisconnectMembers",
            "ViewAuditLog",
            "ApproveMembers",
        ])
    );
    assert_eq!(ADMIN_PERMISSIONS, names(&["Administrator"]));

    // And as integers, the form the server stores.
    assert_eq!(EVERYONE_PERMISSIONS.bits(), 0x0000_1077_001F_0421);
    assert_eq!(MODERATOR_PERMISSIONS.bits(), 0x0000_0780_00C0_2A90);
    assert_eq!(ADMIN_PERMISSIONS.bits(), 0x4000_0000_0000_0000);
    assert_eq!(NEW_MEMBER_RESTRICTED.bits(), 0x0000_0072_0001_0000);
    assert_eq!(
        NEW_MEMBER_RESTRICTED,
        names(&[
            "Speak",
            "SendMessages",
            "StreamScreen",
            "StreamCamera",
            "UseAuxAudio"
        ])
    );
    assert_eq!(COMMUNITY_NEW_MEMBER_PERIOD_MS, 600_000);

    // The moderator role only adds; it repeats nothing of @everyone.
    assert!(!MODERATOR_PERMISSIONS.intersects(EVERYONE_PERMISSIONS));
    assert!(!MODERATOR_PERMISSIONS.contains(Permissions::ADMINISTRATOR));
    assert!(!EVERYONE_PERMISSIONS.contains(Permissions::ADMINISTRATOR));
}

#[test]
fn the_friends_preset_is_the_table_as_written() {
    let defaults = ServerDefaults::new(Preset::Friends);

    assert_eq!(defaults.everyone.name, "@everyone");
    assert_eq!(defaults.everyone.name, EVERYONE_NAME);
    assert_eq!(defaults.everyone.position, 0);
    assert_eq!(defaults.everyone.permissions, EVERYONE_PERMISSIONS);
    assert_eq!(defaults.everyone.limits, RoleLimits::EVERYONE_DEFAULT);

    assert_eq!(defaults.moderator.name, "Moderator");
    assert_eq!(defaults.moderator.position, 1);
    assert_eq!(defaults.moderator.permissions, MODERATOR_PERMISSIONS);
    assert_eq!(defaults.moderator.limits, RoleLimits::NONE);

    assert_eq!(defaults.admin.name, "Admin");
    assert_eq!(defaults.admin.position, 2);
    assert_eq!(defaults.admin.permissions, ADMIN_PERMISSIONS);
    assert_eq!(defaults.admin.limits, RoleLimits::NONE);

    assert_eq!(
        defaults.new_members,
        NewMemberPolicy {
            period_ms: 0,
            require_role: false,
            restricted: NEW_MEMBER_RESTRICTED
        }
    );
    let roles = defaults.roles();
    assert_eq!(roles.map(|role| role.position), [0, 1, 2]);
    assert_eq!(
        roles.map(|role| role.name),
        ["@everyone", "Moderator", "Admin"]
    );
}

#[test]
fn the_community_preset_tightens_everyone_and_restricts_new_members() {
    let friends = ServerDefaults::new(Preset::Friends);
    let community = ServerDefaults::new(Preset::Community);

    assert_eq!(
        community.everyone.permissions,
        EVERYONE_PERMISSIONS - Permissions::CREATE_JOIN_CODES - Permissions::MENTION_EVERYONE
    );
    assert_eq!(community.everyone.permissions.bits(), 0x0000_1077_001F_0401);
    assert!(
        !community
            .everyone
            .permissions
            .contains(Permissions::MENTION_EVERYONE)
    );
    assert!(
        !community
            .everyone
            .permissions
            .contains(Permissions::CREATE_JOIN_CODES)
    );
    assert_eq!(community.new_members.period_ms, 10 * 60 * 1000);
    assert!(!community.new_members.require_role);
    assert_eq!(community.new_members.restricted, NEW_MEMBER_RESTRICTED);

    // Everything else is the same as in the Friends preset.
    assert_eq!(community.everyone.limits, friends.everyone.limits);
    assert_eq!(community.moderator, friends.moderator);
    assert_eq!(community.admin, friends.admin);
}

/// The default server, instantiated the way the server crate will do it.
struct NewServer {
    everyone: Role,
    moderator: Role,
    admin: Role,
    policy: NewMemberPolicy,
}

impl NewServer {
    fn new(preset: Preset) -> Self {
        let defaults = ServerDefaults::new(preset);
        Self {
            everyone: defaults.everyone.to_role(RoleId(10)),
            moderator: defaults.moderator.to_role(RoleId(11)),
            admin: defaults.admin.to_role(RoleId(12)),
            policy: defaults.new_members,
        }
    }

    fn view(&self) -> ServerView<'_> {
        ServerView {
            new_members: self.policy,
            ..ServerView::new(OWNER, &self.everyone)
        }
    }
}

#[test]
fn a_default_server_behaves_as_the_table_promises() {
    let new = NewServer::new(Preset::Friends);
    let server = new.view();
    assert_eq!(new.everyone.position, 0);
    assert_eq!(new.moderator.id, RoleId(11));

    let moderator_roles: &[&Role] = &[&new.moderator];
    let admin_roles: &[&Role] = &[&new.admin];
    let member = MemberView::new(UserId(2), &[]);
    let other_member = MemberView::new(UserId(3), &[]);
    let moderator = MemberView::new(UserId(4), moderator_roles);
    let other_moderator = MemberView::new(UserId(5), moderator_roles);
    let admin = MemberView::new(UserId(6), admin_roles);
    let owner = MemberView::new(OWNER, &[]);

    assert_eq!(effective(&member, &server, None, NOW), EVERYONE_PERMISSIONS);
    assert_eq!(
        effective(&moderator, &server, None, NOW),
        EVERYONE_PERMISSIONS | MODERATOR_PERMISSIONS
    );
    assert_eq!(effective(&admin, &server, None, NOW), Permissions::ALL);
    assert_eq!(effective(&owner, &server, None, NOW), Permissions::ALL);

    // Kicking: the permission, then the hierarchy.
    let kick = |actor: &MemberView<'_>, target: &MemberView<'_>| {
        require(
            effective(actor, &server, None, NOW),
            Permissions::KICK_MEMBERS,
        )
        .and_then(|()| check_moderate(actor, target, &server))
    };
    assert_eq!(
        kick(&member, &other_member),
        Err(Denied::MissingPermissions(Permissions::KICK_MEMBERS))
    );
    assert_eq!(kick(&moderator, &member), Ok(()));
    assert_eq!(kick(&moderator, &other_moderator), Err(Denied::EqualRank));
    assert_eq!(kick(&moderator, &admin), Err(Denied::TargetNotBelowActor));
    assert_eq!(kick(&moderator, &owner), Err(Denied::OwnerIsTarget));
    assert_eq!(kick(&admin, &moderator), Ok(()));
    assert_eq!(kick(&admin, &owner), Err(Denied::OwnerIsTarget));
    assert_eq!(kick(&owner, &admin), Ok(()));

    // Default moderators do not manage roles; administrators do, below
    // their own role.
    assert_eq!(
        require(
            effective(&moderator, &server, None, NOW),
            Permissions::MANAGE_ROLES
        ),
        Err(Denied::MissingPermissions(Permissions::MANAGE_ROLES))
    );
    assert_eq!(
        check_assign_role(&admin, &member, &server, NOW, &new.moderator),
        Ok(())
    );
    assert_eq!(
        check_assign_role(&admin, &member, &server, NOW, &new.admin),
        Err(Denied::AssignedRoleNotBelowActor)
    );
    assert_eq!(
        check_edit_role(
            &admin,
            &server,
            NOW,
            &new.admin,
            Permissions::EMPTY,
            &RoleLimits::NONE
        ),
        Err(Denied::RoleNotBelowActor)
    );
    assert_eq!(
        check_assign_role(&owner, &member, &server, NOW, &new.admin),
        Ok(())
    );

    // Limits: @everyone carries the defaults; nobody is capped.
    assert_eq!(
        effective_limits(&member, &server, None),
        RoleLimits::EVERYONE_DEFAULT
    );
    assert_eq!(
        effective_limits(&moderator, &server, None),
        RoleLimits::EVERYONE_DEFAULT
    );
    assert_eq!(
        effective_limits(&admin, &server, None),
        RoleLimits::UNLIMITED
    );
}

#[test]
fn a_community_server_restricts_a_member_for_ten_minutes() {
    let new = NewServer::new(Preset::Community);
    let server = new.view();
    let joined = Timestamp(5_000_000);
    let member = MemberView {
        joined_at: joined,
        ..MemberView::new(UserId(2), &[])
    };
    let everyone = new.everyone.permissions;

    // Speaking, sending and streaming are gone, and with SendMessages go
    // attachments and embeds.
    let restricted =
        everyone - NEW_MEMBER_RESTRICTED - Permissions::ATTACH_FILES - Permissions::EMBED_LINKS;
    let cases: &[(&str, u64, Permissions)] = &[
        ("on joining", 0, restricted),
        ("after nine minutes", 9 * 60 * 1000, restricted),
        (
            "one millisecond before ten minutes",
            10 * 60 * 1000 - 1,
            restricted,
        ),
        ("after ten minutes", 10 * 60 * 1000, everyone),
        ("after an hour", 60 * 60 * 1000, everyone),
    ];
    for &(name, age, expected) in cases {
        let now = Timestamp(joined.0 + age);
        assert_eq!(effective(&member, &server, None, now), expected, "{name}");
    }
    assert!(restricted.contains(Permissions::VIEW_CHANNEL | Permissions::CONNECT));
    assert!(restricted.contains(Permissions::READ_MESSAGE_HISTORY | Permissions::ADD_REACTIONS));

    // In the Friends preset nobody is restricted.
    let friends = NewServer::new(Preset::Friends);
    assert_eq!(
        effective(&member, &friends.view(), None, joined),
        EVERYONE_PERMISSIONS
    );
}
