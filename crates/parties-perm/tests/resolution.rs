//! Table-driven tests for every step of permission resolution (section 4)
//! and for the moderation state that changes its result (section 8).

mod common;

use common::{EVERYONE, NOW, OWNER, member_overwrite, role, role_overwrite};
use parties_perm::{
    ChannelId, ChannelView, MemberView, NewMemberPolicy, Overwrite, Permissions, Role, RoleId,
    ServerView, Timestamp, UserId, apply_implications, apply_overwrites, base_permissions,
    can_view, effective, is_administrator,
};

const VIEW: Permissions = Permissions::VIEW_CHANNEL;
const NICK: Permissions = Permissions::CHANGE_NICKNAME;
const KICK: Permissions = Permissions::KICK_MEMBERS;
const MANAGE_ROLES: Permissions = Permissions::MANAGE_ROLES;
const SEND: Permissions = Permissions::SEND_MESSAGES;
const HISTORY: Permissions = Permissions::READ_MESSAGE_HISTORY;
const ATTACH: Permissions = Permissions::ATTACH_FILES;
const EMBED: Permissions = Permissions::EMBED_LINKS;
const REACT: Permissions = Permissions::ADD_REACTIONS;
const MENTION: Permissions = Permissions::MENTION_EVERYONE;
const MANAGE_MESSAGES: Permissions = Permissions::MANAGE_MESSAGES;
const PIN: Permissions = Permissions::PIN_MESSAGES;
const CONNECT: Permissions = Permissions::CONNECT;
const SPEAK: Permissions = Permissions::SPEAK;
const VAD: Permissions = Permissions::USE_VOICE_ACTIVITY;
const PRIORITY: Permissions = Permissions::PRIORITY_SPEAKER;
const SCREEN: Permissions = Permissions::STREAM_SCREEN;
const CAMERA: Permissions = Permissions::STREAM_CAMERA;
const MUTE: Permissions = Permissions::MUTE_MEMBERS;
const ADMIN: Permissions = Permissions::ADMINISTRATOR;
const NONE: Permissions = Permissions::EMPTY;

/// A reserved bit, to check that unknown bits behave like any other.
const BIT_46: Permissions = Permissions::from_bits_truncate(1 << 46);

const MEMBER: UserId = UserId(2);
const OTHER: UserId = UserId(3);
const CHANNEL: ChannelId = ChannelId(500);

/// A row of the owner table: name, `@everyone`, roles, timeout, policy.
type OwnerCase<'a> = (
    &'static str,
    &'a Role,
    &'a [&'a Role],
    Option<Timestamp>,
    NewMemberPolicy,
);

/// A row of the new-member table: name, period, role required, joined at,
/// now, roles, restricted.
type PolicyCase<'a> = (&'static str, u64, bool, u64, u64, &'a [&'a Role], bool);

/// The roles every table uses.
struct Roles {
    everyone: Role,
    streamer: Role,
    muted: Role,
    moderator: Role,
    admin: Role,
}

/// What `@everyone` holds in the fixture.
fn everyone_mask() -> Permissions {
    VIEW | NICK | SEND | HISTORY | ATTACH | EMBED | REACT | CONNECT | SPEAK | VAD
}

fn streamer_mask() -> Permissions {
    SCREEN | CAMERA | MENTION
}

fn moderator_mask() -> Permissions {
    KICK | MANAGE_MESSAGES | MUTE | MANAGE_ROLES
}

fn roles() -> Roles {
    Roles {
        everyone: role(EVERYONE.0, 0, everyone_mask()),
        streamer: role(101, 1, streamer_mask()),
        muted: role(102, 2, NONE),
        moderator: role(103, 3, moderator_mask()),
        admin: role(104, 4, ADMIN),
    }
}

fn server(everyone: &Role) -> ServerView<'_> {
    ServerView::new(OWNER, everyone)
}

/// Effective permissions of the fixture member with these roles: in a
/// channel with these overwrites, or server-wide for `None`.
fn resolve(all: &Roles, held: &[&Role], overwrites: Option<&[Overwrite]>) -> Permissions {
    let member = MemberView::new(MEMBER, held);
    let channel = overwrites.map(|overwrites| ChannelView::new(CHANNEL, overwrites));
    effective(&member, &server(&all.everyone), channel.as_ref(), NOW)
}

fn on_everyone(allow: Permissions, deny: Permissions) -> Overwrite {
    role_overwrite(EVERYONE, allow, deny)
}

fn on_role(role: &Role, allow: Permissions, deny: Permissions) -> Overwrite {
    role_overwrite(role.id, allow, deny)
}

fn on_member(allow: Permissions, deny: Permissions) -> Overwrite {
    member_overwrite(MEMBER, allow, deny)
}

// ---------------------------------------------------------------------------
// The owner
// ---------------------------------------------------------------------------

#[test]
fn the_owner_resolves_to_all() {
    let all = roles();
    let nothing = role(EVERYONE.0, 0, NONE);
    let hostile = [
        on_everyone(NONE, Permissions::ALL),
        member_overwrite(OWNER, NONE, Permissions::ALL),
    ];
    let policy = NewMemberPolicy {
        period_ms: u64::MAX,
        require_role: true,
        restricted: Permissions::ALL,
    };

    let cases: &[OwnerCase<'_>] = &[
        ("no roles", &all.everyone, &[], None, NewMemberPolicy::OFF),
        (
            "everyone holds nothing",
            &nothing,
            &[],
            None,
            NewMemberPolicy::OFF,
        ),
        (
            "with roles",
            &all.everyone,
            &[&all.muted, &all.streamer],
            None,
            NewMemberPolicy::OFF,
        ),
        (
            "timeout on record",
            &all.everyone,
            &[],
            Some(Timestamp(u64::MAX)),
            NewMemberPolicy::OFF,
        ),
        ("restricted as a new member", &nothing, &[], None, policy),
    ];
    for &(name, everyone, held, timeout_until, new_members) in cases {
        let member = MemberView {
            timeout_until,
            ..MemberView::new(OWNER, held)
        };
        let server = ServerView {
            new_members,
            ..ServerView::new(OWNER, everyone)
        };
        let channel = ChannelView::new(CHANNEL, &hostile);

        assert_eq!(
            effective(&member, &server, None, NOW),
            Permissions::ALL,
            "{name}: server"
        );
        assert_eq!(
            effective(&member, &server, Some(&channel), NOW),
            Permissions::ALL,
            "{name}: channel"
        );
        assert!(is_administrator(&member, &server), "{name}");
        assert!(can_view(&member, &server, &channel, NOW), "{name}");
    }

    // Ownership is the user id and nothing else: another member with the
    // same roles is an ordinary member.
    let not_owner = MemberView::new(MEMBER, &[]);
    assert_eq!(effective(&not_owner, &server(&nothing), None, NOW), NONE);
}

// ---------------------------------------------------------------------------
// Step 1: the server-wide base
// ---------------------------------------------------------------------------

#[test]
fn the_base_is_everyone_united_with_every_role() {
    let all = roles();
    let cases: &[(&str, &[&Role], Permissions)] = &[
        ("only @everyone", &[], everyone_mask()),
        ("a role without permissions", &[&all.muted], everyone_mask()),
        (
            "one role",
            &[&all.streamer],
            everyone_mask() | streamer_mask(),
        ),
        (
            "two roles",
            &[&all.streamer, &all.moderator],
            everyone_mask() | streamer_mask() | moderator_mask(),
        ),
        (
            "order does not matter",
            &[&all.moderator, &all.streamer],
            everyone_mask() | streamer_mask() | moderator_mask(),
        ),
        (
            "a role listed twice",
            &[&all.streamer, &all.streamer],
            everyone_mask() | streamer_mask(),
        ),
        ("@everyone listed", &[&all.everyone], everyone_mask()),
        // The base is not expanded for administrators; `effective` does that.
        ("administrator role", &[&all.admin], everyone_mask() | ADMIN),
    ];
    for &(name, held, expected) in cases {
        let member = MemberView::new(MEMBER, held);
        assert_eq!(
            base_permissions(&member, &server(&all.everyone)),
            expected,
            "{name}"
        );
        if !expected.contains(ADMIN) {
            assert_eq!(resolve(&all, held, None), expected, "{name}: effective");
        }
    }
}

#[test]
fn a_role_carries_reserved_bits() {
    let all = roles();
    let future = role(105, 5, BIT_46);
    assert_eq!(resolve(&all, &[&future], None), everyone_mask() | BIT_46);
}

// ---------------------------------------------------------------------------
// Administrator
// ---------------------------------------------------------------------------

#[test]
fn an_administrator_resolves_to_all() {
    let all = roles();
    let admin_everyone = role(EVERYONE.0, 0, ADMIN);
    let bare_admin = role(104, 4, ADMIN);
    let hostile = [
        on_everyone(NONE, Permissions::ALL),
        on_role(&all.admin, NONE, Permissions::ALL),
        on_member(NONE, Permissions::ALL),
    ];
    let policy = NewMemberPolicy {
        period_ms: u64::MAX,
        require_role: true,
        restricted: Permissions::ALL,
    };
    let nothing = role(EVERYONE.0, 0, NONE);

    let cases: &[(&str, &Role, &[&Role])] = &[
        ("through a role", &all.everyone, &[&all.admin]),
        (
            "through a role, among others",
            &all.everyone,
            &[&all.muted, &all.admin, &all.streamer],
        ),
        ("through @everyone", &admin_everyone, &[]),
        (
            "the role holds only Administrator",
            &nothing,
            &[&bare_admin],
        ),
    ];
    for &(name, everyone, held) in cases {
        let member = MemberView {
            timeout_until: Some(Timestamp(u64::MAX)),
            joined_at: NOW,
            ..MemberView::new(MEMBER, held)
        };
        let server = ServerView {
            new_members: policy,
            ..ServerView::new(OWNER, everyone)
        };
        let channel = ChannelView::new(CHANNEL, &hostile);

        assert_eq!(
            effective(&member, &server, None, NOW),
            Permissions::ALL,
            "{name}: server"
        );
        assert_eq!(
            effective(&member, &server, Some(&channel), NOW),
            Permissions::ALL,
            "{name}: channel"
        );
        assert!(is_administrator(&member, &server), "{name}");
    }

    let moderator_roles: &[&Role] = &[&all.moderator];
    let member = MemberView::new(MEMBER, moderator_roles);
    assert!(!is_administrator(&member, &server(&all.everyone)));
}

#[test]
fn an_overwrite_cannot_make_an_administrator() {
    let all = roles();
    let overwrites = [
        on_everyone(ADMIN, NONE),
        on_role(&all.muted, ADMIN, NONE),
        on_member(ADMIN, NONE),
    ];
    assert_eq!(
        resolve(&all, &[&all.muted], Some(&overwrites)),
        everyone_mask()
    );
}

// ---------------------------------------------------------------------------
// Step 2: overwrites
// ---------------------------------------------------------------------------

#[test]
fn a_channel_without_overwrites_changes_nothing() {
    let all = roles();
    for held in [
        &[][..],
        &[&all.streamer][..],
        &[&all.moderator, &all.muted][..],
    ] {
        assert_eq!(resolve(&all, held, Some(&[])), resolve(&all, held, None));
    }
}

#[test]
fn overwrites_apply_least_specific_first() {
    let all = roles();
    let base = everyone_mask();
    let with_streamer = base | streamer_mask();

    let cases: &[(&str, &[&Role], &[Overwrite], Permissions)] = &[
        // The @everyone overwrite.
        (
            "everyone: deny removes",
            &[],
            &[on_everyone(NONE, REACT | SPEAK)],
            base - REACT - SPEAK,
        ),
        (
            "everyone: allow adds",
            &[],
            &[on_everyone(PIN | SCREEN, NONE)],
            base | PIN | SCREEN,
        ),
        (
            "everyone: allow and deny in one overwrite",
            &[],
            &[on_everyone(PIN, REACT)],
            (base | PIN) - REACT,
        ),
        (
            "everyone: denying an absent bit",
            &[],
            &[on_everyone(NONE, PIN)],
            base,
        ),
        (
            "everyone: allowing a held bit",
            &[],
            &[on_everyone(SPEAK, NONE)],
            base,
        ),
        // Role overwrites.
        (
            "role: deny removes",
            &[&all.muted],
            &[on_role(&all.muted, NONE, SPEAK | REACT)],
            base - SPEAK - REACT,
        ),
        (
            "role: allow adds",
            &[&all.muted],
            &[on_role(&all.muted, PIN, NONE)],
            base | PIN,
        ),
        (
            "role: allow beats the everyone deny",
            &[&all.streamer],
            &[
                on_everyone(NONE, SPEAK),
                on_role(&all.streamer, SPEAK, NONE),
            ],
            with_streamer,
        ),
        (
            "role: deny beats the everyone allow",
            &[&all.muted],
            &[on_everyone(PIN, NONE), on_role(&all.muted, NONE, PIN)],
            base,
        ),
        (
            "roles: allow wins over deny",
            &[&all.streamer, &all.muted],
            &[
                on_role(&all.muted, NONE, SPEAK | REACT),
                on_role(&all.streamer, SPEAK, NONE),
            ],
            with_streamer - REACT,
        ),
        (
            "roles: allow wins over deny, whatever the order of the overwrites",
            &[&all.muted, &all.streamer],
            &[
                on_role(&all.streamer, SPEAK, NONE),
                on_role(&all.muted, NONE, SPEAK | REACT),
            ],
            with_streamer - REACT,
        ),
        (
            "role: an overwrite for a role the member lacks is ignored",
            &[&all.streamer],
            &[
                on_role(&all.muted, NONE, VIEW | SPEAK),
                on_role(&all.moderator, PIN, NONE),
            ],
            with_streamer,
        ),
        // The member overwrite.
        (
            "member: deny removes",
            &[],
            &[on_member(NONE, SPEAK)],
            base - SPEAK,
        ),
        (
            "member: allow adds",
            &[],
            &[on_member(PIN, NONE)],
            base | PIN,
        ),
        (
            "member: deny beats a role allow",
            &[&all.streamer],
            &[
                on_role(&all.streamer, PRIORITY, NONE),
                on_member(NONE, PRIORITY | SCREEN),
            ],
            with_streamer - SCREEN,
        ),
        (
            "member: allow beats role and everyone denies",
            &[&all.muted],
            &[
                on_everyone(NONE, SPEAK),
                on_role(&all.muted, NONE, SPEAK | REACT),
                on_member(SPEAK | REACT, NONE),
            ],
            base,
        ),
        (
            "member: deny beats an everyone allow",
            &[],
            &[on_member(NONE, PIN), on_everyone(PIN, NONE)],
            base,
        ),
        (
            "member: an overwrite for someone else is ignored",
            &[],
            &[member_overwrite(OTHER, PIN, VIEW)],
            base,
        ),
        // Reserved bits follow the same rules.
        (
            "reserved bit: allowed to everyone, denied to the member",
            &[],
            &[on_everyone(BIT_46, NONE), on_member(NONE, BIT_46)],
            base,
        ),
        (
            "reserved bit: allowed to a role",
            &[&all.muted],
            &[on_role(&all.muted, BIT_46, NONE)],
            base | BIT_46,
        ),
    ];
    for &(name, held, overwrites, expected) in cases {
        assert_eq!(resolve(&all, held, Some(overwrites)), expected, "{name}");

        let reversed: Vec<Overwrite> = overwrites.iter().rev().copied().collect();
        assert_eq!(
            resolve(&all, held, Some(&reversed)),
            expected,
            "{name}, reversed"
        );
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Tri {
    Inherit,
    Allow,
    Deny,
}

const TRI: [Tri; 3] = [Tri::Inherit, Tri::Allow, Tri::Deny];

/// For one bit, every combination of base and overwrite states on the four
/// levels (everyone, two roles, member), checked against the rule in words:
/// the most specific level that says something decides, and between the two
/// roles an allow wins.
#[test]
fn overwrite_precedence_is_exhaustive() {
    let role_a = role(101, 1, NONE);
    let role_b = role(102, 2, NONE);
    let held = [&role_a, &role_b];
    let mut checked = 0;

    for bit in Permissions::ALL {
        let server_scope = Permissions::SERVER_SCOPE.contains(bit);
        for base_has in [false, true] {
            let everyone = role(EVERYONE.0, 0, if base_has { bit } else { NONE });
            let server = server(&everyone);
            let member = MemberView::new(MEMBER, &held);
            for e in TRI {
                for a in TRI {
                    for b in TRI {
                        for m in TRI {
                            let mut overwrites = Vec::new();
                            let mut push =
                                |tri: Tri, target: fn(Permissions, Permissions) -> Overwrite| {
                                    match tri {
                                        Tri::Inherit => {}
                                        Tri::Allow => overwrites.push(target(bit, NONE)),
                                        Tri::Deny => overwrites.push(target(NONE, bit)),
                                    }
                                };
                            push(e, |allow, deny| role_overwrite(EVERYONE, allow, deny));
                            push(a, |allow, deny| role_overwrite(RoleId(101), allow, deny));
                            push(b, |allow, deny| role_overwrite(RoleId(102), allow, deny));
                            push(m, |allow, deny| member_overwrite(MEMBER, allow, deny));

                            let mut expected = base_has;
                            if !server_scope {
                                match e {
                                    Tri::Allow => expected = true,
                                    Tri::Deny => expected = false,
                                    Tri::Inherit => {}
                                }
                                if a == Tri::Allow || b == Tri::Allow {
                                    expected = true;
                                } else if a == Tri::Deny || b == Tri::Deny {
                                    expected = false;
                                }
                                match m {
                                    Tri::Allow => expected = true,
                                    Tri::Deny => expected = false,
                                    Tri::Inherit => {}
                                }
                            }

                            let channel = ChannelView::new(CHANNEL, &overwrites);
                            let base = base_permissions(&member, &server);
                            let raw = apply_overwrites(base, &member, &server, &channel);
                            assert_eq!(
                                raw,
                                if expected { bit } else { NONE },
                                "bit {bit}, base {base_has}, everyone {e:?}, roles {a:?} {b:?}, member {m:?}"
                            );
                            checked += 1;
                        }
                    }
                }
            }
        }
    }
    assert_eq!(checked, 63 * 2 * 81);
}

#[test]
fn overwrites_cannot_grant_or_remove_a_server_scope_bit() {
    let all = roles();
    let base = everyone_mask() | moderator_mask();
    let held: &[&Role] = &[&all.moderator];

    let cases: &[(&str, &[Overwrite])] = &[
        (
            "everyone allows",
            &[on_everyone(
                Permissions::BAN_MEMBERS | Permissions::MANAGE_SERVER,
                NONE,
            )],
        ),
        ("everyone denies", &[on_everyone(NONE, KICK | NICK)]),
        (
            "role allows",
            &[on_role(&all.moderator, Permissions::BAN_MEMBERS, NONE)],
        ),
        ("role denies", &[on_role(&all.moderator, NONE, KICK)]),
        (
            "member allows",
            &[on_member(Permissions::VIEW_AUDIT_LOG, NONE)],
        ),
        ("member denies", &[on_member(NONE, KICK | NICK)]),
        (
            "every server-scope bit allowed",
            &[on_member(Permissions::SERVER_SCOPE, NONE)],
        ),
        (
            "every server-scope bit denied",
            &[on_member(NONE, Permissions::SERVER_SCOPE)],
        ),
    ];
    for &(name, overwrites) in cases {
        assert_eq!(resolve(&all, held, Some(overwrites)), base, "{name}");
    }

    // A channel-scope bit in the same overwrite still applies.
    let mixed = [on_member(Permissions::BAN_MEMBERS | PIN, KICK | SPEAK)];
    assert_eq!(resolve(&all, held, Some(&mixed)), (base | PIN) - SPEAK);
}

#[test]
fn listing_everyone_among_the_roles_changes_nothing() {
    let all = roles();
    // If the @everyone overwrite were counted again on the role level, its
    // allow would beat the denial of `muted`.
    let overwrites = [on_everyone(PIN, NONE), on_role(&all.muted, NONE, PIN)];
    let without = resolve(&all, &[&all.muted], Some(&overwrites));
    let with = resolve(&all, &[&all.everyone, &all.muted], Some(&overwrites));
    assert_eq!(without, everyone_mask());
    assert_eq!(with, without);
}

#[test]
fn duplicate_overwrites_for_one_target_fail_closed() {
    let all = roles();
    // Storage allows one overwrite per target. Should two rows ever reach
    // resolution, a denial on the @everyone or the member level wins.
    let everyone_twice = [on_everyone(PIN, NONE), on_everyone(NONE, PIN | SPEAK)];
    assert_eq!(
        resolve(&all, &[], Some(&everyone_twice)),
        everyone_mask() - SPEAK
    );

    let member_twice = [on_member(NONE, PIN | SPEAK), on_member(PIN, NONE)];
    assert_eq!(
        resolve(&all, &[], Some(&member_twice)),
        everyone_mask() - SPEAK
    );
}

// ---------------------------------------------------------------------------
// Step 3: implied restrictions
// ---------------------------------------------------------------------------

#[test]
fn implied_restrictions() {
    let everyone = role(EVERYONE.0, 0, NONE);
    let voice = Permissions::VOICE_VIDEO;
    let text = Permissions::TEXT;
    let ordinary = Permissions::ALL - ADMIN;
    let restricted = SPEAK | SEND | SCREEN;

    struct Row {
        name: &'static str,
        input: Permissions,
        in_channel: bool,
        timed_out: bool,
        restricted: Permissions,
        expect: Permissions,
    }
    let row = |name, input, in_channel, expect| Row {
        name,
        input,
        in_channel,
        timed_out: false,
        restricted: NONE,
        expect,
    };

    let rows = [
        row(
            "nothing is implied when the three anchors are held",
            ordinary,
            true,
            ordinary,
        ),
        row("the empty set stays empty", NONE, false, NONE),
        // No ViewChannel.
        row(
            "no ViewChannel in a channel: nothing remains",
            ordinary - VIEW,
            true,
            NONE,
        ),
        row(
            "no ViewChannel in a channel: server-scope bits go too",
            KICK | NICK,
            true,
            NONE,
        ),
        row(
            "no ViewChannel in a channel: reserved bits go too",
            BIT_46 | SEND,
            true,
            NONE,
        ),
        row(
            "no ViewChannel server-wide: no effect",
            ordinary - VIEW,
            false,
            ordinary - VIEW,
        ),
        row("ViewChannel alone, in a channel", VIEW, true, VIEW),
        // No Connect.
        row(
            "no Connect in a channel: the voice group goes",
            ordinary - CONNECT,
            true,
            ordinary - voice,
        ),
        row(
            "no Connect server-wide: the voice group goes",
            ordinary - CONNECT,
            false,
            ordinary - voice,
        ),
        row(
            "no Connect: every voice bit, nothing else",
            VIEW | (voice - CONNECT) | KICK | BIT_46,
            true,
            VIEW | KICK | BIT_46,
        ),
        row(
            "Connect held: the voice group stays",
            VIEW | voice,
            true,
            VIEW | voice,
        ),
        // No SendMessages.
        row(
            "no SendMessages: attachments, embeds and mass mentions go",
            ordinary - SEND,
            true,
            ordinary - SEND - ATTACH - EMBED - MENTION,
        ),
        row(
            "no SendMessages server-wide: the same",
            ordinary - SEND,
            false,
            ordinary - SEND - ATTACH - EMBED - MENTION,
        ),
        row(
            "no SendMessages: the other text bits stay",
            VIEW | (text - SEND),
            true,
            VIEW | HISTORY | REACT | MANAGE_MESSAGES | PIN | Permissions::BYPASS_SLOW_MODE,
        ),
        row(
            "SendMessages held: the text group stays",
            VIEW | text,
            true,
            VIEW | text,
        ),
        // Timed out.
        Row {
            name: "timed out in a channel: ViewChannel and ReadMessageHistory remain",
            input: ordinary,
            in_channel: true,
            timed_out: true,
            restricted: NONE,
            expect: VIEW | HISTORY,
        },
        Row {
            name: "timed out server-wide: the same",
            input: ordinary,
            in_channel: false,
            timed_out: true,
            restricted: NONE,
            expect: VIEW | HISTORY,
        },
        Row {
            name: "timed out without ReadMessageHistory",
            input: ordinary - HISTORY,
            in_channel: true,
            timed_out: true,
            restricted: NONE,
            expect: VIEW,
        },
        Row {
            name: "timed out without ViewChannel in a channel",
            input: ordinary - VIEW,
            in_channel: true,
            timed_out: true,
            restricted: NONE,
            expect: NONE,
        },
        Row {
            name: "timed out without ViewChannel server-wide",
            input: ordinary - VIEW,
            in_channel: false,
            timed_out: true,
            restricted: NONE,
            expect: HISTORY,
        },
        // New member.
        Row {
            name: "new member: the listed bits go, and what depends on them",
            input: ordinary,
            in_channel: true,
            timed_out: false,
            restricted,
            expect: ordinary - restricted - ATTACH - EMBED - MENTION,
        },
        Row {
            name: "new member: a restriction on Connect takes the voice group",
            input: ordinary,
            in_channel: true,
            timed_out: false,
            restricted: CONNECT,
            expect: ordinary - voice,
        },
        Row {
            name: "new member: a restriction on ViewChannel hides the channel",
            input: ordinary,
            in_channel: true,
            timed_out: false,
            restricted: VIEW,
            expect: NONE,
        },
        Row {
            name: "new member: a restriction on ViewChannel, server-wide",
            input: ordinary,
            in_channel: false,
            timed_out: false,
            restricted: VIEW,
            expect: ordinary - VIEW,
        },
        Row {
            name: "new member and timed out",
            input: ordinary,
            in_channel: true,
            timed_out: true,
            restricted: HISTORY,
            expect: VIEW,
        },
    ];

    for row in rows {
        let member = MemberView {
            joined_at: NOW,
            timeout_until: row.timed_out.then_some(Timestamp(NOW.0 + 1)),
            ..MemberView::new(MEMBER, &[])
        };
        // A period of 1 ms restricts a member who joined at `NOW`. An empty
        // list of restricted bits makes the policy a no-op.
        let policy = NewMemberPolicy {
            period_ms: 1,
            require_role: false,
            restricted: row.restricted,
        };
        let server = ServerView {
            new_members: policy,
            ..ServerView::new(OWNER, &everyone)
        };

        let actual = apply_implications(row.input, &member, &server, row.in_channel, NOW);
        assert_eq!(actual, row.expect, "{}", row.name);

        // Applying the restrictions again changes nothing.
        let again = apply_implications(actual, &member, &server, row.in_channel, NOW);
        assert_eq!(again, actual, "{}: not idempotent", row.name);
    }
}

#[test]
fn implied_restrictions_through_effective() {
    let all = roles();
    let held: &[&Role] = &[&all.streamer, &all.moderator];
    let base = everyone_mask() | streamer_mask() | moderator_mask();

    let cases: &[(&str, &[Overwrite], Permissions)] = &[
        ("ViewChannel denied", &[on_everyone(NONE, VIEW)], NONE),
        (
            "ViewChannel denied, allowed back to a role",
            &[on_everyone(NONE, VIEW), on_role(&all.moderator, VIEW, NONE)],
            base,
        ),
        (
            "Connect denied",
            &[on_everyone(NONE, CONNECT)],
            base - CONNECT - SPEAK - VAD - SCREEN - CAMERA - MUTE,
        ),
        (
            "SendMessages denied",
            &[on_member(NONE, SEND)],
            base - SEND - ATTACH - EMBED - MENTION,
        ),
        (
            "an allowed bit does not survive its missing anchor",
            &[on_member(SCREEN | ATTACH, CONNECT | SEND)],
            base - CONNECT - SPEAK - VAD - SCREEN - CAMERA - MUTE - SEND - ATTACH - EMBED - MENTION,
        ),
    ];
    for &(name, overwrites, expected) in cases {
        assert_eq!(resolve(&all, held, Some(overwrites)), expected, "{name}");
    }

    // Server-wide, a missing ViewChannel removes nothing else.
    let blind = role(EVERYONE.0, 0, everyone_mask() - VIEW);
    let moderator_roles: &[&Role] = &[&all.moderator];
    let member = MemberView::new(MEMBER, moderator_roles);
    assert_eq!(
        effective(&member, &server(&blind), None, NOW),
        (everyone_mask() | moderator_mask()) - VIEW
    );
    // In any channel it removes everything, unless an overwrite gives the
    // bit back there.
    let channel = ChannelView::new(CHANNEL, &[]);
    assert_eq!(
        effective(&member, &server(&blind), Some(&channel), NOW),
        NONE
    );
    let opened = [on_role(&all.moderator, VIEW, NONE)];
    let channel = ChannelView::new(CHANNEL, &opened);
    assert_eq!(
        effective(&member, &server(&blind), Some(&channel), NOW),
        everyone_mask() | moderator_mask()
    );
}

// ---------------------------------------------------------------------------
// Moderation state: timeout
// ---------------------------------------------------------------------------

#[test]
fn a_timeout_ends_at_its_end_instant() {
    let cases: &[(&str, Option<u64>, u64, bool)] = &[
        ("no timeout", None, 1000, false),
        ("one millisecond before the end", Some(1000), 999, true),
        ("at the end", Some(1000), 1000, false),
        ("one millisecond after the end", Some(1000), 1001, false),
        ("long before the end", Some(1000), 0, true),
        ("a timeout until zero never holds", Some(0), 0, false),
        (
            "the largest end instant",
            Some(u64::MAX),
            u64::MAX - 1,
            true,
        ),
        ("the largest instant", Some(u64::MAX), u64::MAX, false),
    ];
    let all = roles();
    for &(name, until, now, timed_out) in cases {
        let member = MemberView {
            timeout_until: until.map(Timestamp),
            ..MemberView::new(MEMBER, &[])
        };
        assert_eq!(member.is_timed_out(Timestamp(now)), timed_out, "{name}");

        let expected = if timed_out {
            VIEW | HISTORY
        } else {
            everyone_mask()
        };
        assert_eq!(
            effective(&member, &server(&all.everyone), None, Timestamp(now)),
            expected,
            "{name}: effective"
        );
    }
}

#[test]
fn a_timed_out_member_is_read_only_everywhere() {
    let all = roles();
    let held: &[&Role] = &[&all.moderator, &all.streamer];
    let member = MemberView {
        timeout_until: Some(Timestamp(NOW.0 + 1)),
        ..MemberView::new(MEMBER, held)
    };
    let server = server(&all.everyone);

    let cases: &[(&str, Option<&[Overwrite]>, Permissions)] = &[
        ("server-wide", None, VIEW | HISTORY),
        ("plain channel", Some(&[]), VIEW | HISTORY),
        (
            "a channel that allows more",
            Some(&[on_member(PIN | SPEAK | SEND, NONE)]),
            VIEW | HISTORY,
        ),
        (
            "a channel without history",
            Some(&[on_everyone(NONE, HISTORY)]),
            VIEW,
        ),
        ("a hidden channel", Some(&[on_everyone(NONE, VIEW)]), NONE),
    ];
    for &(name, overwrites, expected) in cases {
        let channel = overwrites.map(|overwrites| ChannelView::new(CHANNEL, overwrites));
        assert_eq!(
            effective(&member, &server, channel.as_ref(), NOW),
            expected,
            "{name}"
        );
    }
}

// ---------------------------------------------------------------------------
// Moderation state: new members
// ---------------------------------------------------------------------------

#[test]
fn the_new_member_policy_applies_by_age_or_by_missing_role() {
    let muted = role(102, 2, NONE);
    let everyone = role(EVERYONE.0, 0, NONE);
    let no_roles: &[&Role] = &[];
    let one_role: &[&Role] = &[&muted];
    let only_everyone: &[&Role] = &[&everyone];

    // (name, period, require_role, joined_at, now, roles, restricted?)
    let cases: &[PolicyCase<'_>] = &[
        ("policy off", 0, false, 1000, 1000, no_roles, false),
        ("just joined", 600, false, 1000, 1000, no_roles, true),
        (
            "one millisecond before the period ends",
            600,
            false,
            1000,
            1599,
            no_roles,
            true,
        ),
        (
            "exactly the period",
            600,
            false,
            1000,
            1600,
            no_roles,
            false,
        ),
        (
            "one millisecond after the period",
            600,
            false,
            1000,
            1601,
            no_roles,
            false,
        ),
        ("long after", 600, false, 0, 1_000_000, no_roles, false),
        (
            "the join time is in the future",
            600,
            false,
            2000,
            1000,
            no_roles,
            true,
        ),
        (
            "a period of one millisecond, same instant",
            1,
            false,
            1000,
            1000,
            no_roles,
            true,
        ),
        (
            "a period of one millisecond, next instant",
            1,
            false,
            1000,
            1001,
            no_roles,
            false,
        ),
        (
            "a role does not shorten the period",
            600,
            false,
            1000,
            1000,
            one_role,
            true,
        ),
        (
            "role required, none held",
            0,
            true,
            0,
            1_000_000,
            no_roles,
            true,
        ),
        (
            "role required, one held",
            0,
            true,
            0,
            1_000_000,
            one_role,
            false,
        ),
        (
            "role required, only @everyone listed",
            0,
            true,
            0,
            1_000_000,
            only_everyone,
            true,
        ),
        (
            "role required and young, role held",
            600,
            true,
            1000,
            1000,
            one_role,
            true,
        ),
        (
            "role required and old, role held",
            600,
            true,
            1000,
            1600,
            one_role,
            false,
        ),
        (
            "role required and old, none held",
            600,
            true,
            1000,
            1600,
            no_roles,
            true,
        ),
        (
            "no role but none required",
            600,
            false,
            1000,
            1600,
            no_roles,
            false,
        ),
    ];
    for &(name, period_ms, require_role, joined_at, now, held, restricted) in cases {
        let policy = NewMemberPolicy {
            period_ms,
            require_role,
            restricted: SPEAK,
        };
        let member = MemberView {
            joined_at: Timestamp(joined_at),
            ..MemberView::new(MEMBER, held)
        };
        assert_eq!(
            policy.applies_to(&member, EVERYONE, Timestamp(now)),
            restricted,
            "{name}"
        );

        let speaking = role(EVERYONE.0, 0, VIEW | CONNECT | SPEAK);
        let server = ServerView {
            new_members: policy,
            ..ServerView::new(OWNER, &speaking)
        };
        let expected = if restricted {
            VIEW | CONNECT
        } else {
            VIEW | CONNECT | SPEAK
        };
        assert_eq!(
            effective(&member, &server, None, Timestamp(now)),
            expected,
            "{name}: effective"
        );
    }
}

#[test]
fn a_restricted_member_cannot_be_given_the_bits_back_by_an_overwrite() {
    let all = roles();
    let policy = NewMemberPolicy {
        period_ms: 600_000,
        require_role: false,
        restricted: SPEAK | SEND,
    };
    let server = ServerView {
        new_members: policy,
        ..ServerView::new(OWNER, &all.everyone)
    };
    let member = MemberView {
        joined_at: NOW,
        ..MemberView::new(MEMBER, &[])
    };
    let overwrites = [
        on_everyone(SPEAK | SEND, NONE),
        on_member(SPEAK | SEND | ATTACH, NONE),
    ];
    let channel = ChannelView::new(CHANNEL, &overwrites);

    assert_eq!(
        effective(&member, &server, Some(&channel), NOW),
        everyone_mask() - SPEAK - SEND - ATTACH - EMBED
    );
}

// ---------------------------------------------------------------------------
// Moderation state: the temporary speak grant
// ---------------------------------------------------------------------------

#[test]
fn a_speak_grant_lets_a_member_speak_in_one_moderated_channel() {
    let all = roles();
    let server = server(&all.everyone);
    let base = everyone_mask();
    let moderated = [on_everyone(NONE, SPEAK)];
    let other_channel = ChannelId(501);

    struct Case<'a> {
        name: &'static str,
        grant: Option<ChannelId>,
        timeout: bool,
        channel: Option<ChannelId>,
        overwrites: &'a [Overwrite],
        expect: Permissions,
    }
    let cases = [
        Case {
            name: "no grant",
            grant: None,
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &moderated,
            expect: base - SPEAK,
        },
        Case {
            name: "grant for this channel",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &moderated,
            expect: base,
        },
        Case {
            name: "grant for another channel",
            grant: Some(other_channel),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &moderated,
            expect: base - SPEAK,
        },
        Case {
            name: "the grant does not leak into another channel",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(other_channel),
            overwrites: &moderated,
            expect: base - SPEAK,
        },
        Case {
            name: "the grant has no effect server-wide",
            grant: Some(CHANNEL),
            timeout: false,
            channel: None,
            overwrites: &[],
            expect: base,
        },
        Case {
            name: "a member overwrite that denies Speak wins",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &[on_everyone(NONE, SPEAK), on_member(NONE, SPEAK)],
            expect: base - SPEAK,
        },
        Case {
            name: "no Connect, no Speak",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &[on_everyone(NONE, SPEAK | CONNECT)],
            expect: base - SPEAK - CONNECT - VAD,
        },
        Case {
            name: "no ViewChannel, nothing",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &[on_everyone(NONE, SPEAK | VIEW)],
            expect: NONE,
        },
        Case {
            name: "a timeout wins",
            grant: Some(CHANNEL),
            timeout: true,
            channel: Some(CHANNEL),
            overwrites: &moderated,
            expect: VIEW | HISTORY,
        },
        Case {
            name: "the grant gives Speak and nothing else",
            grant: Some(CHANNEL),
            timeout: false,
            channel: Some(CHANNEL),
            overwrites: &[on_everyone(NONE, SPEAK | VAD | SEND)],
            expect: base - VAD - SEND - ATTACH - EMBED,
        },
    ];
    for case in cases {
        let member = MemberView {
            speak_grant: case.grant,
            timeout_until: case.timeout.then_some(Timestamp(NOW.0 + 1)),
            ..MemberView::new(MEMBER, &[])
        };
        let channel = case.channel.map(|id| ChannelView::new(id, case.overwrites));
        assert_eq!(
            effective(&member, &server, channel.as_ref(), NOW),
            case.expect,
            "{}",
            case.name
        );
    }
}

// ---------------------------------------------------------------------------
// Visibility and composition
// ---------------------------------------------------------------------------

#[test]
fn visibility_follows_view_channel() {
    let all = roles();
    let server = server(&all.everyone);
    let hidden = [on_everyone(NONE, VIEW)];
    let hidden_but_staff = [on_everyone(NONE, VIEW), on_role(&all.moderator, VIEW, NONE)];

    let plain = MemberView::new(MEMBER, &[]);
    let moderator_roles: &[&Role] = &[&all.moderator];
    let moderator = MemberView::new(MEMBER, moderator_roles);
    let admin_roles: &[&Role] = &[&all.admin];
    let admin = MemberView::new(MEMBER, admin_roles);
    let owner = MemberView::new(OWNER, &[]);
    let timed_out = MemberView {
        timeout_until: Some(Timestamp(NOW.0 + 1)),
        ..plain
    };

    let cases: &[(&str, &MemberView<'_>, &[Overwrite], bool)] = &[
        ("open channel, member", &plain, &[], true),
        ("hidden channel, member", &plain, &hidden, false),
        ("hidden channel, moderator", &moderator, &hidden, false),
        ("staff channel, member", &plain, &hidden_but_staff, false),
        (
            "staff channel, moderator",
            &moderator,
            &hidden_but_staff,
            true,
        ),
        ("hidden channel, administrator", &admin, &hidden, true),
        ("hidden channel, owner", &owner, &hidden, true),
        ("open channel, timed-out member", &timed_out, &[], true),
        (
            "hidden channel, timed-out member",
            &timed_out,
            &hidden,
            false,
        ),
    ];
    for &(name, member, overwrites, visible) in cases {
        let channel = ChannelView::new(CHANNEL, overwrites);
        assert_eq!(can_view(member, &server, &channel, NOW), visible, "{name}");
        if !visible {
            // A hidden channel grants nothing at all.
            assert_eq!(
                effective(member, &server, Some(&channel), NOW),
                NONE,
                "{name}"
            );
        }
    }
}

#[test]
fn effective_is_the_three_steps_in_order() {
    let all = roles();
    let held: &[&Role] = &[&all.streamer, &all.muted];
    let overwrites = [
        on_everyone(PIN, SPEAK),
        on_role(&all.muted, NONE, SEND | PIN),
        on_role(&all.streamer, SPEAK, NONE),
        on_member(PRIORITY, CAMERA),
    ];
    let member = MemberView::new(MEMBER, held);
    let server = server(&all.everyone);
    let channel = ChannelView::new(CHANNEL, &overwrites);

    let base = base_permissions(&member, &server);
    assert_eq!(base, everyone_mask() | streamer_mask());

    let raw = apply_overwrites(base, &member, &server, &channel);
    assert_eq!(raw, (base | PRIORITY) - SEND - CAMERA);

    let implied = apply_implications(raw, &member, &server, true, NOW);
    assert_eq!(implied, raw - ATTACH - EMBED - MENTION);

    assert_eq!(effective(&member, &server, Some(&channel), NOW), implied);
}
