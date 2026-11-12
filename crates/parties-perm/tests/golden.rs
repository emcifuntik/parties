//! Golden vectors for resolution (section 14): hand-written cases in JSON,
//! shared with any future client that is not written in Rust.
//!
//! The file format is described in the `about` field of
//! `tests/vectors/resolution.json`. The reader below is strict: an unknown
//! key or permission name fails the test instead of being skipped.

use std::collections::BTreeMap;

use parties_perm::{
    ChannelId, ChannelView, MemberView, NewMemberPolicy, Overwrite, OverwriteTarget, Permissions,
    Role, RoleId, RoleLimits, ServerView, Timestamp, UserId, effective,
};
use serde_json::{Map, Value};

struct Server {
    owner: UserId,
    everyone: Role,
    roles: Vec<Role>,
    policy: NewMemberPolicy,
}

fn object<'a>(value: &'a Value, allowed: &[&str], what: &str) -> &'a Map<String, Value> {
    let map = value
        .as_object()
        .unwrap_or_else(|| panic!("{what}: expected an object"));
    for key in map.keys() {
        assert!(
            allowed.contains(&key.as_str()),
            "{what}: unknown key '{key}'"
        );
    }
    map
}

fn number(map: &Map<String, Value>, key: &str, what: &str) -> u64 {
    map.get(key)
        .and_then(Value::as_u64)
        .unwrap_or_else(|| panic!("{what}: '{key}' must be a non-negative integer"))
}

fn optional_number(map: &Map<String, Value>, key: &str, what: &str) -> Option<u64> {
    match map.get(key) {
        None | Some(Value::Null) => None,
        Some(value) => Some(
            value
                .as_u64()
                .unwrap_or_else(|| panic!("{what}: '{key}' must be an integer or null")),
        ),
    }
}

fn permission_list(value: Option<&Value>, what: &str) -> Permissions {
    let Some(value) = value else {
        return Permissions::EMPTY;
    };
    let names = value
        .as_array()
        .unwrap_or_else(|| panic!("{what}: expected a list of names"));
    names
        .iter()
        .map(|name| {
            let name = name
                .as_str()
                .unwrap_or_else(|| panic!("{what}: names are strings"));
            match name.strip_prefix('#') {
                Some(bit) => {
                    let bit: u32 = bit
                        .parse()
                        .unwrap_or_else(|_| panic!("{what}: bad bit {name}"));
                    assert!(bit < 63, "{what}: bit {bit} does not exist");
                    Permissions::from_bits(1u64 << bit).unwrap()
                }
                None => Permissions::from_name(name)
                    .unwrap_or_else(|| panic!("{what}: unknown permission '{name}'")),
            }
        })
        .collect()
}

fn read_role(value: &Value, position: Option<u32>, what: &str) -> Role {
    let keys: &[&str] = if position.is_some() {
        &["id", "permissions"]
    } else {
        &["id", "position", "permissions"]
    };
    let map = object(value, keys, what);
    Role {
        id: RoleId(number(map, "id", what)),
        position: position.unwrap_or_else(|| u32::try_from(number(map, "position", what)).unwrap()),
        permissions: permission_list(map.get("permissions"), what),
        limits: RoleLimits::NONE,
    }
}

fn read_server(value: &Value, what: &str) -> Server {
    let map = object(
        value,
        &["owner_id", "everyone", "roles", "new_members"],
        what,
    );
    let policy = object(
        &map["new_members"],
        &["period_ms", "require_role", "restricted"],
        what,
    );
    Server {
        owner: UserId(number(map, "owner_id", what)),
        everyone: read_role(&map["everyone"], Some(0), what),
        roles: map["roles"]
            .as_array()
            .unwrap_or_else(|| panic!("{what}: 'roles' must be a list"))
            .iter()
            .map(|role| read_role(role, None, what))
            .collect(),
        policy: NewMemberPolicy {
            period_ms: number(policy, "period_ms", what),
            require_role: policy["require_role"]
                .as_bool()
                .expect("require_role is a boolean"),
            restricted: permission_list(policy.get("restricted"), what),
        },
    }
}

fn read_overwrite(value: &Value, what: &str) -> Overwrite {
    let map = object(value, &["role", "member", "allow", "deny"], what);
    let target = match (
        optional_number(map, "role", what),
        optional_number(map, "member", what),
    ) {
        (Some(role), None) => OverwriteTarget::Role(RoleId(role)),
        (None, Some(member)) => OverwriteTarget::Member(UserId(member)),
        _ => panic!("{what}: an overwrite targets either 'role' or 'member'"),
    };
    Overwrite::new(
        target,
        permission_list(map.get("allow"), what),
        permission_list(map.get("deny"), what),
    )
    .unwrap_or_else(|error| panic!("{what}: {error}"))
}

#[test]
fn resolution_matches_the_golden_vectors() {
    let file: Value = serde_json::from_str(include_str!("vectors/resolution.json")).unwrap();
    let root = object(&file, &["format", "about", "servers", "cases"], "file");
    assert_eq!(root["format"], 1);

    let servers: BTreeMap<&str, Server> = root["servers"]
        .as_object()
        .unwrap()
        .iter()
        .map(|(name, value)| {
            (
                name.as_str(),
                read_server(value, &format!("server '{name}'")),
            )
        })
        .collect();

    let cases = root["cases"].as_array().unwrap();
    assert!(cases.len() >= 45, "cases went missing: {}", cases.len());

    let mut names = Vec::new();
    let mut failures = Vec::new();
    for case in cases {
        let name = case["name"].as_str().expect("every case has a name");
        let what = format!("case '{name}'");
        assert!(!names.contains(&name), "{what}: duplicate name");
        names.push(name);

        let map = object(
            case,
            &["name", "server", "now", "member", "channel", "expect"],
            &what,
        );
        let server_name = map["server"].as_str().expect("server is a name");
        let server = servers
            .get(server_name)
            .unwrap_or_else(|| panic!("{what}: unknown server"));
        let now = Timestamp(number(map, "now", &what));

        let member = object(
            &map["member"],
            &[
                "user_id",
                "roles",
                "joined_at",
                "timeout_until",
                "speak_grant",
            ],
            &what,
        );
        let roles: Vec<&Role> = member["roles"]
            .as_array()
            .unwrap_or_else(|| panic!("{what}: member.roles must be a list"))
            .iter()
            .map(|id| {
                let id = RoleId(id.as_u64().expect("role ids are integers"));
                std::iter::once(&server.everyone)
                    .chain(&server.roles)
                    .find(|role| role.id == id)
                    .unwrap_or_else(|| panic!("{what}: the server has no role {}", id.0))
            })
            .collect();
        let member_view = MemberView {
            user_id: UserId(number(member, "user_id", &what)),
            roles: &roles,
            joined_at: Timestamp(optional_number(member, "joined_at", &what).unwrap_or(0)),
            timeout_until: optional_number(member, "timeout_until", &what).map(Timestamp),
            speak_grant: optional_number(member, "speak_grant", &what).map(ChannelId),
        };
        let server_view = ServerView {
            owner_id: server.owner,
            everyone: &server.everyone,
            limit_caps: RoleLimits::UNLIMITED,
            new_members: server.policy,
        };

        let channel = match map.get("channel") {
            None | Some(Value::Null) => None,
            Some(value) => {
                let channel = object(value, &["id", "overwrites"], &what);
                let overwrites: Vec<Overwrite> = channel["overwrites"]
                    .as_array()
                    .unwrap_or_else(|| panic!("{what}: channel.overwrites must be a list"))
                    .iter()
                    .map(|overwrite| read_overwrite(overwrite, &what))
                    .collect();
                Some((ChannelId(number(channel, "id", &what)), overwrites))
            }
        };
        let channel_view = channel
            .as_ref()
            .map(|(id, overwrites)| ChannelView::new(*id, overwrites));

        let expected = match &map["expect"] {
            Value::String(all) if all == "ALL" => Permissions::ALL,
            list @ Value::Array(_) => permission_list(Some(list), &what),
            _ => panic!("{what}: 'expect' is a list of names or \"ALL\""),
        };
        let actual = effective(&member_view, &server_view, channel_view.as_ref(), now);
        if actual != expected {
            failures.push(format!(
                "{what}:\n    expected {expected}\n    actual   {actual}\n    missing  {}\n    extra    {}",
                expected - actual,
                actual - expected
            ));
        }
    }
    assert!(
        failures.is_empty(),
        "{} of {} cases failed:\n{}",
        failures.len(),
        cases.len(),
        failures.join("\n")
    );

    // Every row of the table of implied restrictions has at least one case.
    for restriction in [
        "ViewChannel",
        "Connect",
        "SendMessages",
        "timed-out",
        "new member",
    ] {
        assert!(
            names
                .iter()
                .any(|name| name.starts_with("implied:") && name.contains(restriction)),
            "no case for the implied restriction on {restriction}"
        );
    }
}
