//! Table-driven tests for the numeric role limits (section 6).

mod common;

use common::{EVERYONE, OWNER};
use parties_perm::{
    ChannelId, ChannelView, LimitKind, MemberView, Permissions, Role, RoleId, RoleLimits,
    ServerView, UserId, effective_limits, limit_grant_ceiling, role_limits,
};

const MEMBER: UserId = UserId(2);

/// Limits with one value for the video bitrate and one for the upload size;
/// two kinds are enough to see that each is treated on its own.
fn limits(bitrate: u32, upload: u32) -> RoleLimits {
    RoleLimits::NONE
        .with(LimitKind::MaxVideoBitrate, bitrate)
        .with(LimitKind::MaxUploadSize, upload)
}

fn role(id: u64, position: u32, permissions: Permissions, limits: RoleLimits) -> Role {
    Role {
        id: RoleId(id),
        position,
        permissions,
        limits,
    }
}

#[test]
fn names_units_and_defaults_are_pinned() {
    // The names are keys in stored and transmitted limits.
    let table: &[(LimitKind, &str, &str, u32)] = &[
        (
            LimitKind::MaxVideoBitrate,
            "max_video_bitrate",
            "kbit/s",
            8000,
        ),
        (
            LimitKind::MaxVideoHeight,
            "max_video_height",
            "pixels",
            1080,
        ),
        (LimitKind::MaxVideoFps, "max_video_fps", "frames/s", 60),
        (
            LimitKind::MaxAuxAudioBitrate,
            "max_aux_audio_bitrate",
            "kbit/s",
            128,
        ),
        (LimitKind::MaxUploadSize, "max_upload_size", "MiB", 50),
        (
            LimitKind::DailyUploadQuota,
            "daily_upload_quota",
            "MiB",
            500,
        ),
        (
            LimitKind::MaxPublishedVideoTracks,
            "max_published_video_tracks",
            "count",
            2,
        ),
    ];
    assert_eq!(LimitKind::ALL.len(), table.len());
    for (&(kind, name, unit, default), listed) in table.iter().zip(LimitKind::ALL) {
        assert_eq!(listed, kind, "order of LimitKind::ALL");
        assert_eq!(kind.name(), name);
        assert_eq!(kind.unit(), unit, "{name}");
        assert_eq!(LimitKind::from_name(name), Some(kind));
        assert_eq!(RoleLimits::EVERYONE_DEFAULT.get(kind), default, "{name}");
        assert_eq!(RoleLimits::NONE.get(kind), 0, "{name}");
        assert_eq!(RoleLimits::UNLIMITED.get(kind), u32::MAX, "{name}");
    }
    assert_eq!(LimitKind::from_name("max_video_bitrate "), None);
    assert_eq!(LimitKind::from_name("MaxVideoBitrate"), None);
    assert_eq!(RoleLimits::default(), RoleLimits::NONE);
}

#[test]
fn each_limit_is_read_and_written_on_its_own() {
    for (index, kind) in LimitKind::ALL.into_iter().enumerate() {
        let value = 100 + u32::try_from(index).unwrap();
        let mut written = RoleLimits::NONE;
        written.set(kind, value);
        assert_eq!(written, RoleLimits::NONE.with(kind, value));
        for other in LimitKind::ALL {
            assert_eq!(
                written.get(other),
                if other == kind { value } else { 0 },
                "{kind:?} / {other:?}"
            );
        }
    }
    // The fields and the kinds name the same values.
    let named = RoleLimits {
        max_video_bitrate: 1,
        max_video_height: 2,
        max_video_fps: 3,
        max_aux_audio_bitrate: 4,
        max_upload_size: 5,
        daily_upload_quota: 6,
        max_published_video_tracks: 7,
    };
    let values: Vec<u32> = LimitKind::ALL
        .into_iter()
        .map(|kind| named.get(kind))
        .collect();
    assert_eq!(values, [1, 2, 3, 4, 5, 6, 7]);
}

#[test]
fn max_min_and_fits_within_work_per_limit() {
    let a = limits(8000, 10);
    let b = limits(4000, 50);
    assert_eq!(a.max(b), limits(8000, 50));
    assert_eq!(a.min(b), limits(4000, 10));
    assert_eq!(a.max(RoleLimits::NONE), a);
    assert_eq!(a.min(RoleLimits::UNLIMITED), a);
    assert_eq!(a.max(RoleLimits::UNLIMITED), RoleLimits::UNLIMITED);
    assert_eq!(a.min(RoleLimits::NONE), RoleLimits::NONE);

    assert!(a.fits_within(&a));
    assert!(a.fits_within(&limits(8000, 11)));
    assert!(!a.fits_within(&limits(7999, 10)));
    assert!(!a.fits_within(&b));
    assert!(!b.fits_within(&a));
    assert!(RoleLimits::NONE.fits_within(&a));
    assert!(a.fits_within(&RoleLimits::UNLIMITED));
    for kind in LimitKind::ALL {
        let one_more = RoleLimits::NONE.with(kind, 1);
        assert!(!one_more.fits_within(&RoleLimits::NONE), "{kind:?}");
        assert_eq!(one_more.max(RoleLimits::NONE), one_more, "{kind:?}");
        assert_eq!(one_more.min(RoleLimits::UNLIMITED), one_more, "{kind:?}");
    }
}

#[test]
fn a_members_limit_is_the_maximum_over_their_roles() {
    let everyone = role(EVERYONE.0, 0, Permissions::EMPTY, limits(8000, 50));
    let unset = role(101, 1, Permissions::EMPTY, RoleLimits::NONE);
    let streamer = role(102, 2, Permissions::EMPTY, limits(20_000, 0));
    let uploader = role(103, 3, Permissions::EMPTY, limits(1000, 500));
    let server = ServerView::new(OWNER, &everyone);

    let cases: &[(&str, &[&Role], RoleLimits)] = &[
        ("only @everyone", &[], limits(8000, 50)),
        ("a role that sets nothing", &[&unset], limits(8000, 50)),
        ("a role raises one limit", &[&streamer], limits(20_000, 50)),
        (
            "a lower value in a role does not lower the limit",
            &[&uploader],
            limits(8000, 500),
        ),
        (
            "each limit takes its own maximum",
            &[&streamer, &uploader],
            limits(20_000, 500),
        ),
        (
            "order does not matter",
            &[&uploader, &unset, &streamer],
            limits(20_000, 500),
        ),
    ];
    for &(name, held, expected) in cases {
        let member = MemberView::new(MEMBER, held);
        assert_eq!(role_limits(&member, &server), expected, "{name}");
        // Without caps the applied value and the grant ceiling are the same.
        assert_eq!(
            effective_limits(&member, &server, None),
            expected,
            "{name}: effective"
        );
        assert_eq!(
            limit_grant_ceiling(&member, &server),
            expected,
            "{name}: ceiling"
        );
    }
}

#[test]
fn the_applied_value_is_capped_by_server_and_channel() {
    let everyone = role(EVERYONE.0, 0, Permissions::EMPTY, limits(8000, 50));
    let streamer = role(102, 2, Permissions::EMPTY, limits(20_000, 0));
    let held: &[&Role] = &[&streamer];
    let member = MemberView::new(MEMBER, held);
    let no_cap = RoleLimits::UNLIMITED;

    // (name, server cap, channel cap, expected); own limits are (20000, 50).
    let cases: &[(&str, RoleLimits, Option<RoleLimits>, RoleLimits)] = &[
        ("no caps", no_cap, None, limits(20_000, 50)),
        (
            "server cap above",
            no_cap.with(LimitKind::MaxVideoBitrate, 20_001),
            None,
            limits(20_000, 50),
        ),
        (
            "server cap equal",
            no_cap.with(LimitKind::MaxVideoBitrate, 20_000),
            None,
            limits(20_000, 50),
        ),
        (
            "server cap one below",
            no_cap.with(LimitKind::MaxVideoBitrate, 19_999),
            None,
            limits(19_999, 50),
        ),
        (
            "server cap on another limit",
            no_cap.with(LimitKind::MaxUploadSize, 10),
            None,
            limits(20_000, 10),
        ),
        (
            "channel without a cap",
            no_cap,
            Some(no_cap),
            limits(20_000, 50),
        ),
        (
            "channel cap below",
            no_cap,
            Some(no_cap.with(LimitKind::MaxVideoBitrate, 2500)),
            limits(2500, 50),
        ),
        (
            "channel cap above",
            no_cap,
            Some(no_cap.with(LimitKind::MaxVideoBitrate, 30_000)),
            limits(20_000, 50),
        ),
        (
            "the lower of the two caps wins: channel",
            no_cap.with(LimitKind::MaxVideoBitrate, 6000),
            Some(no_cap.with(LimitKind::MaxVideoBitrate, 2500)),
            limits(2500, 50),
        ),
        (
            "the lower of the two caps wins: server",
            no_cap.with(LimitKind::MaxVideoBitrate, 2000),
            Some(no_cap.with(LimitKind::MaxVideoBitrate, 2500)),
            limits(2000, 50),
        ),
        ("a cap of zero", RoleLimits::NONE, None, RoleLimits::NONE),
    ];
    for &(name, server_cap, channel_cap, expected) in cases {
        let server = ServerView {
            limit_caps: server_cap,
            ..ServerView::new(OWNER, &everyone)
        };
        let channel = channel_cap.map(|limit_caps| ChannelView {
            limit_caps,
            ..ChannelView::new(ChannelId(500), &[])
        });
        assert_eq!(
            effective_limits(&member, &server, channel.as_ref()),
            expected,
            "{name}"
        );
        // Caps never change what the member holds or may grant.
        assert_eq!(
            role_limits(&member, &server),
            limits(20_000, 50),
            "{name}: own"
        );
        assert_eq!(
            limit_grant_ceiling(&member, &server),
            limits(20_000, 50),
            "{name}: ceiling"
        );
    }
}

#[test]
fn owner_and_administrators_get_the_server_wide_cap() {
    let everyone = role(EVERYONE.0, 0, Permissions::EMPTY, limits(8000, 50));
    let admin_role = role(104, 4, Permissions::ADMINISTRATOR, RoleLimits::NONE);
    let admin_everyone = role(EVERYONE.0, 0, Permissions::ADMINISTRATOR, limits(8000, 50));
    let cap = limits(30_000, 2000);
    let channel_cap = RoleLimits::UNLIMITED.with(LimitKind::MaxVideoBitrate, 2500);
    let channel = ChannelView {
        limit_caps: channel_cap,
        ..ChannelView::new(ChannelId(500), &[])
    };
    let admin_roles: &[&Role] = &[&admin_role];

    let cases: &[(&str, &Role, UserId, &[&Role])] = &[
        ("owner", &everyone, OWNER, &[]),
        (
            "administrator through a role",
            &everyone,
            MEMBER,
            admin_roles,
        ),
        (
            "administrator through @everyone",
            &admin_everyone,
            MEMBER,
            &[],
        ),
    ];
    for &(name, everyone, user, held) in cases {
        let server = ServerView {
            limit_caps: cap,
            ..ServerView::new(OWNER, everyone)
        };
        let member = MemberView::new(user, held);

        assert_eq!(
            effective_limits(&member, &server, None),
            cap,
            "{name}: server-wide"
        );
        // The channel's own cap still applies.
        assert_eq!(
            effective_limits(&member, &server, Some(&channel)),
            cap.with(LimitKind::MaxVideoBitrate, 2500),
            "{name}: in a capped channel"
        );
        // Their own roles do not bound them.
        assert_eq!(
            role_limits(&member, &server),
            limits(8000, 50),
            "{name}: role limits"
        );
        assert_eq!(
            limit_grant_ceiling(&member, &server),
            RoleLimits::UNLIMITED,
            "{name}: ceiling"
        );
    }

    // An ordinary member on the same server keeps their own limits.
    let server = ServerView {
        limit_caps: cap,
        ..ServerView::new(OWNER, &everyone)
    };
    let member = MemberView::new(MEMBER, &[]);
    assert_eq!(effective_limits(&member, &server, None), limits(8000, 50));
    assert_eq!(
        effective_limits(&member, &server, Some(&channel)),
        limits(2500, 50)
    );
}
