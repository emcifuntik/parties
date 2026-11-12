//! The permission catalogue (section 3): every bit value is pinned here.
//!
//! The integer form of a permission set is stored in SQLite and sent on the
//! wire. If one of these tests fails, the change breaks existing servers.

use parties_perm::PermissionGroup::{General, Special, Text, VoiceVideo};
use parties_perm::PermissionScope::{Channel, Server};
use parties_perm::{CATALOGUE, PermissionGroup, PermissionScope, Permissions};

/// One line per permission, copied from the table in the design document:
/// name, constant, bit index, integer value, group, scope.
#[rustfmt::skip]
const PINNED: &[(&str, Permissions, u8, u64, PermissionGroup, PermissionScope)] = &[
    ("ViewChannel",        Permissions::VIEW_CHANNEL,          0, 0x0000_0000_0000_0001, General,    Channel),
    ("ManageChannels",     Permissions::MANAGE_CHANNELS,       1, 0x0000_0000_0000_0002, General,    Channel),
    ("ManageRoles",        Permissions::MANAGE_ROLES,          2, 0x0000_0000_0000_0004, General,    Channel),
    ("ManageServer",       Permissions::MANAGE_SERVER,         3, 0x0000_0000_0000_0008, General,    Server),
    ("ViewAuditLog",       Permissions::VIEW_AUDIT_LOG,        4, 0x0000_0000_0000_0010, General,    Server),
    ("CreateJoinCodes",    Permissions::CREATE_JOIN_CODES,     5, 0x0000_0000_0000_0020, General,    Channel),
    ("ManageJoinCodes",    Permissions::MANAGE_JOIN_CODES,     6, 0x0000_0000_0000_0040, General,    Server),
    ("KickMembers",        Permissions::KICK_MEMBERS,          7, 0x0000_0000_0000_0080, General,    Server),
    ("BanMembers",         Permissions::BAN_MEMBERS,           8, 0x0000_0000_0000_0100, General,    Server),
    ("TimeoutMembers",     Permissions::TIMEOUT_MEMBERS,       9, 0x0000_0000_0000_0200, General,    Server),
    ("ChangeNickname",     Permissions::CHANGE_NICKNAME,      10, 0x0000_0000_0000_0400, General,    Server),
    ("ManageNicknames",    Permissions::MANAGE_NICKNAMES,     11, 0x0000_0000_0000_0800, General,    Server),
    ("ManageExpressions",  Permissions::MANAGE_EXPRESSIONS,   12, 0x0000_0000_0000_1000, General,    Server),
    ("ApproveMembers",     Permissions::APPROVE_MEMBERS,      13, 0x0000_0000_0000_2000, General,    Server),
    ("SendMessages",       Permissions::SEND_MESSAGES,        16, 0x0000_0000_0001_0000, Text,       Channel),
    ("ReadMessageHistory", Permissions::READ_MESSAGE_HISTORY, 17, 0x0000_0000_0002_0000, Text,       Channel),
    ("AttachFiles",        Permissions::ATTACH_FILES,         18, 0x0000_0000_0004_0000, Text,       Channel),
    ("EmbedLinks",         Permissions::EMBED_LINKS,          19, 0x0000_0000_0008_0000, Text,       Channel),
    ("AddReactions",       Permissions::ADD_REACTIONS,        20, 0x0000_0000_0010_0000, Text,       Channel),
    ("MentionEveryone",    Permissions::MENTION_EVERYONE,     21, 0x0000_0000_0020_0000, Text,       Channel),
    ("ManageMessages",     Permissions::MANAGE_MESSAGES,      22, 0x0000_0000_0040_0000, Text,       Channel),
    ("PinMessages",        Permissions::PIN_MESSAGES,         23, 0x0000_0000_0080_0000, Text,       Channel),
    ("BypassSlowMode",     Permissions::BYPASS_SLOW_MODE,     24, 0x0000_0000_0100_0000, Text,       Channel),
    ("Connect",            Permissions::CONNECT,              32, 0x0000_0001_0000_0000, VoiceVideo, Channel),
    ("Speak",              Permissions::SPEAK,                33, 0x0000_0002_0000_0000, VoiceVideo, Channel),
    ("UseVoiceActivity",   Permissions::USE_VOICE_ACTIVITY,   34, 0x0000_0004_0000_0000, VoiceVideo, Channel),
    ("PrioritySpeaker",    Permissions::PRIORITY_SPEAKER,     35, 0x0000_0008_0000_0000, VoiceVideo, Channel),
    ("StreamScreen",       Permissions::STREAM_SCREEN,        36, 0x0000_0010_0000_0000, VoiceVideo, Channel),
    ("StreamCamera",       Permissions::STREAM_CAMERA,        37, 0x0000_0020_0000_0000, VoiceVideo, Channel),
    ("UseAuxAudio",        Permissions::USE_AUX_AUDIO,        38, 0x0000_0040_0000_0000, VoiceVideo, Channel),
    ("MuteMembers",        Permissions::MUTE_MEMBERS,         39, 0x0000_0080_0000_0000, VoiceVideo, Channel),
    ("DeafenMembers",      Permissions::DEAFEN_MEMBERS,       40, 0x0000_0100_0000_0000, VoiceVideo, Channel),
    ("MoveMembers",        Permissions::MOVE_MEMBERS,         41, 0x0000_0200_0000_0000, VoiceVideo, Channel),
    ("DisconnectMembers",  Permissions::DISCONNECT_MEMBERS,   42, 0x0000_0400_0000_0000, VoiceVideo, Channel),
    ("BypassUserLimit",    Permissions::BYPASS_USER_LIMIT,    43, 0x0000_0800_0000_0000, VoiceVideo, Channel),
    ("RequestToSpeak",     Permissions::REQUEST_TO_SPEAK,     44, 0x0000_1000_0000_0000, VoiceVideo, Channel),
    ("SetChannelStatus",   Permissions::SET_CHANNEL_STATUS,   45, 0x0000_2000_0000_0000, VoiceVideo, Channel),
    ("Administrator",      Permissions::ADMINISTRATOR,        62, 0x4000_0000_0000_0000, Special,    Server),
];

#[test]
fn every_bit_value_is_pinned() {
    assert_eq!(PINNED.len(), 38);
    assert_eq!(
        CATALOGUE.len(),
        PINNED.len(),
        "a permission was added or removed"
    );
    for (&(name, constant, bit, value, group, scope), info) in PINNED.iter().zip(CATALOGUE) {
        assert_eq!(constant.bits(), value, "{name}: integer value");
        assert_eq!(value, 1u64 << bit, "{name}: the pinned row is inconsistent");
        assert_eq!(
            constant.to_i64(),
            i64::try_from(value).unwrap(),
            "{name}: signed value"
        );
        assert_eq!(info.name, name, "catalogue order at bit {bit}");
        assert_eq!(info.flag, constant, "{name}: catalogue flag");
        assert_eq!(info.bit, bit, "{name}: catalogue bit");
        assert_eq!(info.group, group, "{name}: group");
        assert_eq!(info.scope, scope, "{name}: scope");
        assert!(!info.grants.is_empty(), "{name}: description");
        assert_eq!(
            Permissions::from_name(name),
            Some(constant),
            "{name}: lookup by name"
        );
        assert_eq!(constant.name(), Some(name), "{name}: name of the constant");
        assert_eq!(constant.info(), Some(info), "{name}: info of the constant");
    }
}

#[test]
fn catalogue_is_sorted_and_unambiguous() {
    for pair in CATALOGUE.windows(2) {
        assert!(
            pair[0].bit < pair[1].bit,
            "{} and {}",
            pair[0].name,
            pair[1].name
        );
    }
    for (index, info) in CATALOGUE.iter().enumerate() {
        assert_eq!(info.flag.count(), 1, "{}", info.name);
        assert!(info.bit <= 62, "{}", info.name);
        assert!(
            CATALOGUE[..index]
                .iter()
                .all(|other| other.name != info.name),
            "{}",
            info.name
        );
    }
}

#[test]
fn derived_masks_are_pinned() {
    assert_eq!(Permissions::EMPTY.bits(), 0);
    assert_eq!(Permissions::ALL.bits(), 0x7FFF_FFFF_FFFF_FFFF);
    assert_eq!(Permissions::KNOWN.bits(), 0x4000_3FFF_01FF_3FFF);
    assert_eq!(Permissions::RESERVED.bits(), 0x3FFF_C000_FE00_C000);
    assert_eq!(Permissions::SERVER_SCOPE.bits(), 0x4000_0000_0000_3FD8);
    assert_eq!(Permissions::CHANNEL_SCOPE.bits(), 0x0000_3FFF_01FF_0027);
    assert_eq!(Permissions::GENERAL.bits(), 0x0000_0000_0000_3FFF);
    assert_eq!(Permissions::TEXT.bits(), 0x0000_0000_01FF_0000);
    assert_eq!(Permissions::VOICE_VIDEO.bits(), 0x0000_3FFF_0000_0000);
    assert_eq!(
        Permissions::REQUIRES_SEND_MESSAGES.bits(),
        0x0000_0000_002C_0000
    );
    assert_eq!(Permissions::KEPT_IN_TIMEOUT.bits(), 0x0000_0000_0002_0001);
}

#[test]
fn groups_cover_the_catalogue() {
    assert_eq!(PermissionGroup::ALL, [General, Text, VoiceVideo, Special]);
    assert_eq!(General.title(), "General");
    assert_eq!(Text.title(), "Text");
    assert_eq!(VoiceVideo.title(), "Voice and video");
    assert_eq!(Special.title(), "Special");
    assert_eq!(General.mask(), Permissions::GENERAL);
    assert_eq!(Text.mask(), Permissions::TEXT);
    assert_eq!(VoiceVideo.mask(), Permissions::VOICE_VIDEO);
    assert_eq!(Special.mask(), Permissions::ADMINISTRATOR);

    let union = PermissionGroup::ALL
        .into_iter()
        .map(PermissionGroup::mask)
        .collect::<Permissions>();
    assert_eq!(union, Permissions::KNOWN);
    for (index, group) in PermissionGroup::ALL.into_iter().enumerate() {
        for other in &PermissionGroup::ALL[..index] {
            assert!(
                !group.mask().intersects(other.mask()),
                "{group:?} and {other:?} overlap"
            );
        }
    }
    assert_eq!(
        Permissions::SERVER_SCOPE | Permissions::CHANNEL_SCOPE,
        Permissions::KNOWN
    );
    assert!(!Permissions::SERVER_SCOPE.intersects(Permissions::CHANNEL_SCOPE));
    assert_eq!(Permissions::KNOWN | Permissions::RESERVED, Permissions::ALL);
    assert!(!Permissions::KNOWN.intersects(Permissions::RESERVED));
}

#[test]
fn reserved_bits_are_exactly_the_documented_ranges() {
    let documented = (14..=15).chain(25..=31).chain(46..=61);
    let reserved = documented.fold(0u64, |mask, bit| mask | 1u64 << bit);
    assert_eq!(Permissions::RESERVED.bits(), reserved);
    for bit in 0..63 {
        let flag = Permissions::from_bits(1u64 << bit).unwrap();
        let known = PINNED.iter().any(|row| row.2 == bit);
        assert_eq!(Permissions::KNOWN.contains(flag), known, "bit {bit}");
        assert_eq!(Permissions::RESERVED.contains(flag), !known, "bit {bit}");
        assert_eq!(flag.info().is_some(), known, "bit {bit}");
    }
}

#[test]
fn bit_63_is_never_set() {
    let top = 1u64 << 63;
    assert_eq!(Permissions::from_bits(top), None);
    assert_eq!(Permissions::from_bits(u64::MAX), None);
    assert_eq!(Permissions::from_bits(top - 1), Some(Permissions::ALL));
    assert_eq!(Permissions::from_bits_truncate(u64::MAX), Permissions::ALL);
    assert_eq!(Permissions::from_bits_truncate(top), Permissions::EMPTY);
    assert_eq!(!Permissions::EMPTY, Permissions::ALL);
    assert_eq!(!Permissions::ALL, Permissions::EMPTY);
    assert_eq!((!Permissions::VIEW_CHANNEL).bits(), 0x7FFF_FFFF_FFFF_FFFE);
    assert_eq!(Permissions::ALL.to_i64(), i64::MAX);
    assert_eq!(Permissions::from_i64(i64::MAX), Some(Permissions::ALL));
    assert_eq!(Permissions::from_i64(0), Some(Permissions::EMPTY));
    assert_eq!(Permissions::from_i64(-1), None);
    assert_eq!(Permissions::from_i64(i64::MIN), None);
}

#[test]
fn unknown_bits_are_kept() {
    // Bit 46 is reserved. A role that carries it must come back unchanged.
    let stored = 0x0000_4000_0000_0001_u64;
    let mask = Permissions::from_bits(stored).unwrap();
    assert_eq!(mask.bits(), stored);
    assert_eq!(Permissions::from_i64(mask.to_i64()), Some(mask));
    assert!(mask.contains(Permissions::VIEW_CHANNEL));
    assert_eq!(
        (mask | Permissions::SPEAK).bits(),
        stored | 0x0000_0002_0000_0000
    );
    assert_eq!(
        (mask - Permissions::VIEW_CHANNEL).bits(),
        0x0000_4000_0000_0000
    );
    assert_eq!(
        Permissions::from_bits(0x0000_4000_0000_0000)
            .unwrap()
            .info(),
        None
    );
    assert_eq!(
        Permissions::from_bits(0x0000_4000_0000_0000)
            .unwrap()
            .name(),
        None
    );
}

#[test]
fn set_operations() {
    let view = Permissions::VIEW_CHANNEL;
    let speak = Permissions::SPEAK;
    let both = view | speak;

    assert!(Permissions::EMPTY.is_empty());
    assert!(!view.is_empty());
    assert_eq!(Permissions::default(), Permissions::EMPTY);
    assert_eq!(both.bits(), 0x0000_0002_0000_0001);
    assert_eq!(both.count(), 2);

    assert!(both.contains(view));
    assert!(both.contains(both));
    assert!(both.contains(Permissions::EMPTY));
    assert!(!view.contains(both));
    assert!(both.intersects(view));
    assert!(!view.intersects(speak));
    assert!(!view.intersects(Permissions::EMPTY));

    assert_eq!(both & view, view);
    assert_eq!(both - view, speak);
    assert_eq!(view - both, Permissions::EMPTY);
    assert_eq!(both ^ view, speak);
    assert_eq!(view ^ speak, both);
    assert_eq!(view.union(speak), both);
    assert_eq!(both.intersection(speak), speak);
    assert_eq!(both.difference(speak), view);
    assert_eq!(both.symmetric_difference(speak), view);
    assert_eq!(both.complement() | both, Permissions::ALL);
    assert!(!both.complement().intersects(both));

    let mut mask = Permissions::EMPTY;
    mask |= both;
    assert_eq!(mask, both);
    mask &= view;
    assert_eq!(mask, view);
    mask ^= both;
    assert_eq!(mask, speak);
    mask -= speak;
    assert_eq!(mask, Permissions::EMPTY);
}

#[test]
fn iteration_yields_single_bits_lowest_first() {
    let mask = Permissions::ADMINISTRATOR | Permissions::SPEAK | Permissions::VIEW_CHANNEL;
    let bits: Vec<Permissions> = mask.iter().collect();
    assert_eq!(
        bits,
        [
            Permissions::VIEW_CHANNEL,
            Permissions::SPEAK,
            Permissions::ADMINISTRATOR
        ]
    );
    assert_eq!(mask.iter().len(), 3);
    assert_eq!(mask.into_iter().collect::<Permissions>(), mask);
    assert_eq!(Permissions::EMPTY.iter().next(), None);
    assert_eq!(Permissions::ALL.iter().count(), 63);
    assert_eq!(Permissions::KNOWN.iter().count(), CATALOGUE.len());
}

#[test]
fn names_are_case_sensitive_and_exact() {
    assert_eq!(Permissions::from_name("Speak"), Some(Permissions::SPEAK));
    assert_eq!(Permissions::from_name("speak"), None);
    assert_eq!(Permissions::from_name("SPEAK"), None);
    assert_eq!(Permissions::from_name(""), None);
    assert_eq!(Permissions::from_name("Speak "), None);
    assert_eq!(Permissions::EMPTY.name(), None);
    assert_eq!((Permissions::SPEAK | Permissions::CONNECT).name(), None);
}

#[test]
fn display_lists_names() {
    assert_eq!(Permissions::EMPTY.to_string(), "(none)");
    assert_eq!(Permissions::ALL.to_string(), "ALL");
    assert_eq!(Permissions::SPEAK.to_string(), "Speak");
    assert_eq!(
        (Permissions::SPEAK | Permissions::VIEW_CHANNEL | Permissions::ADMINISTRATOR).to_string(),
        "ViewChannel | Speak | Administrator"
    );
    let with_reserved = Permissions::from_bits(0x0000_4000_0000_8001).unwrap();
    assert_eq!(with_reserved.to_string(), "ViewChannel | 0x400000008000");
    assert_eq!(
        Permissions::from_bits(0x8000).unwrap().to_string(),
        "0x8000"
    );
    assert_eq!(format!("{:?}", Permissions::SPEAK), "Permissions(Speak)");
}

/// The same table as a JSON file, for clients that are not written in Rust.
#[test]
fn golden_bit_table_matches_the_catalogue() {
    let file: serde_json::Value =
        serde_json::from_str(include_str!("vectors/permission_bits.json")).unwrap();
    assert_eq!(file["format"], 1);
    assert_eq!(file["all"], "0x7fffffffffffffff");

    let rows = file["permissions"].as_array().unwrap();
    assert_eq!(rows.len(), CATALOGUE.len());
    for (row, info) in rows.iter().zip(CATALOGUE) {
        assert_eq!(row["name"], info.name);
        assert_eq!(row["bit"], info.bit, "{}", info.name);
        assert_eq!(row["group"], info.group.title(), "{}", info.name);
        let scope = match info.scope {
            Server => "S",
            Channel => "C",
        };
        assert_eq!(row["scope"], scope, "{}", info.name);
    }

    let reserved: Vec<u64> = file["reserved_bits"]
        .as_array()
        .unwrap()
        .iter()
        .map(|bit| bit.as_u64().unwrap())
        .collect();
    let expected: Vec<u64> = Permissions::RESERVED
        .iter()
        .map(|flag| u64::from(flag.bits().trailing_zeros()))
        .collect();
    assert_eq!(reserved, expected);
}
