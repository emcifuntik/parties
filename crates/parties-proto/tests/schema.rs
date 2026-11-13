//! Keeps the three forms of the schema in step, with no tool outside the
//! workspace:
//!
//! 1. `proto/` is the source: the `.proto` files of package `parties.v2`.
//! 2. `src/generated/parties.v2.rs` is the Rust code `prost-build` generates
//!    from them. It is checked in; [`generated_code_is_current`] fails when
//!    it is stale.
//! 3. `schema/parties.v2.snapshot` lists every message, field, enum and
//!    value. [`schema_has_no_breaking_change`] fails when the `.proto` files
//!    remove, renumber, rename or retype anything the snapshot lists. It
//!    stands in for `buf breaking` until CI has that tool
//!    (`docs/rust-rewrite/11-quality-and-release.md` section 1).
//!
//! After a change to `proto/`, regenerate 2 and 3 with
//!
//! ```text
//! PARTIES_PROTO_BLESS=1 cargo test -p parties-proto --test schema
//! ```
//!
//! (PowerShell: `$env:PARTIES_PROTO_BLESS = '1'` first, and remove it
//! afterwards.) That accepts additions only. A deliberate breaking change,
//! which is possible until the schema is frozen for 2.0.0, needs
//! `PARTIES_PROTO_BLESS=breaking`.

// Not `mod common`: this test must build whenever the library does, so that
// the files can be regenerated before fixtures are adapted to a new schema.
#[path = "schema/model.rs"]
mod model;

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use model::{Item, Schema, breaking_changes};
use prost_types::FileDescriptorSet;

const GENERATED: &str = "src/generated/parties.v2.rs";
const SNAPSHOT: &str = "schema/parties.v2.snapshot";
const PACKAGE: &str = "parties.v2";
const BLESS_VARIABLE: &str = "PARTIES_PROTO_BLESS";
const BLESS_HINT: &str =
    "regenerate it with `PARTIES_PROTO_BLESS=1 cargo test -p parties-proto --test schema`";

const GENERATED_HEADER: &str = "\
// Generated from `proto/` by `tests/schema.rs`. Do not edit.
// Regenerate: PARTIES_PROTO_BLESS=1 cargo test -p parties-proto --test schema
";

const SNAPSHOT_HEADER: &str = "\
# The wire schema of package parties.v2: every message, field, enum and value.
# tests/schema.rs compares the .proto files with this list and fails when
# something listed here was removed, renumbered, renamed or retyped.
# Do not edit. Regenerate:
#   PARTIES_PROTO_BLESS=1 cargo test -p parties-proto --test schema
";

/// What `PARTIES_PROTO_BLESS` asks for.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Bless {
    /// Compare only.
    No,
    /// Rewrite the checked-in files, unless the change is breaking.
    Additive,
    /// Rewrite the checked-in files whatever changed.
    Breaking,
}

fn bless() -> Bless {
    match std::env::var(BLESS_VARIABLE) {
        Ok(value) if value == "breaking" => Bless::Breaking,
        Ok(value) if !value.is_empty() && value != "0" => Bless::Additive,
        _ => Bless::No,
    }
}

fn crate_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

/// Git may check text files out with CRLF line endings on Windows.
fn normalized(text: &str) -> String {
    text.replace("\r\n", "\n")
}

fn read(relative: &str) -> String {
    let path = crate_dir().join(relative);
    let text = fs::read_to_string(&path)
        .unwrap_or_else(|error| panic!("cannot read {}: {error}", path.display()));
    normalized(&text)
}

fn write(relative: &str, content: &str) {
    let path = crate_dir().join(relative);
    fs::write(&path, content)
        .unwrap_or_else(|error| panic!("cannot write {}: {error}", path.display()));
}

/// Every `.proto` file under `root`, as a path relative to it with forward
/// slashes, sorted.
fn proto_files(root: &Path) -> Vec<String> {
    fn walk(root: &Path, dir: &Path, found: &mut Vec<String>) {
        let entries = fs::read_dir(dir)
            .unwrap_or_else(|error| panic!("cannot list {}: {error}", dir.display()));
        for entry in entries {
            let path = entry.expect("directory entry").path();
            if path.is_dir() {
                walk(root, &path, found);
            } else if path
                .extension()
                .is_some_and(|extension| extension == "proto")
            {
                let relative = path.strip_prefix(root).expect("path is under the root");
                let name: Vec<_> = relative
                    .components()
                    .map(|part| part.as_os_str().to_str().expect("UTF-8 file name"))
                    .collect();
                found.push(name.join("/"));
            }
        }
    }
    let mut found = Vec::new();
    walk(root, root, &mut found);
    found.sort();
    found
}

/// The `.proto` files, compiled by a compiler written in Rust.
fn descriptors() -> &'static FileDescriptorSet {
    static DESCRIPTORS: OnceLock<FileDescriptorSet> = OnceLock::new();
    DESCRIPTORS.get_or_init(|| {
        let root = crate_dir().join("proto");
        let files = proto_files(&root);
        assert!(!files.is_empty(), "no .proto file under {}", root.display());
        let mut compiler = protox::Compiler::new([&root]).expect("the include path exists");
        compiler.include_source_info(true).include_imports(true);
        if let Err(error) = compiler.open_files(&files) {
            panic!("the .proto files do not compile: {error}");
        }
        compiler.file_descriptor_set()
    })
}

fn current_schema() -> &'static Schema {
    static SCHEMA: OnceLock<Schema> = OnceLock::new();
    SCHEMA.get_or_init(|| Schema::from_descriptors(descriptors()))
}

/// The Rust code `prost-build` generates for the package.
fn generate() -> &'static str {
    static CODE: OnceLock<String> = OnceLock::new();
    CODE.get_or_init(|| {
        let out_dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join("parties-proto-generated");
        fs::create_dir_all(&out_dir).expect("temporary directory");
        let mut config = prost_build::Config::new();
        // `bytes` fields become `bytes::Bytes`: a key, a hash or a proof is
        // then a slice of the received frame, not a copy.
        config.out_dir(&out_dir).bytes(["."]);
        config
            .compile_fds(descriptors().clone())
            .expect("prost-build generates the code");
        let body = fs::read_to_string(out_dir.join(format!("{PACKAGE}.rs")))
            .expect("prost-build wrote one file for the package");
        format!("{GENERATED_HEADER}{}", normalized(&body))
    })
}

/// The breaking changes of the `.proto` files against the checked-in
/// snapshot. A snapshot that does not exist yet has nothing to break.
fn breaking_against_snapshot() -> Vec<String> {
    let path = crate_dir().join(SNAPSHOT);
    if !path.exists() {
        return Vec::new();
    }
    let snapshot = Schema::parse(&read(SNAPSHOT)).expect("the snapshot parses");
    breaking_changes(&snapshot, current_schema())
}

/// In bless mode: panics when the change is breaking and was not asked for,
/// so that nothing is rewritten.
fn refuse_unrequested_break(mode: Bless) {
    if mode == Bless::Additive {
        let breaking = breaking_against_snapshot();
        assert!(
            breaking.is_empty(),
            "refusing to regenerate: the schema has breaking changes. Revert them, or set \
             {BLESS_VARIABLE}=breaking if the break is deliberate.\n  {}",
            breaking.join("\n  ")
        );
    }
}

#[test]
fn schema_has_no_breaking_change() {
    if bless() == Bless::Breaking {
        return;
    }
    let breaking = breaking_against_snapshot();
    assert!(
        breaking.is_empty(),
        "the .proto files break compatibility with {SNAPSHOT}:\n  {}",
        breaking.join("\n  ")
    );
}

#[test]
fn snapshot_is_current() {
    let expected = format!("{SNAPSHOT_HEADER}\n{}", current_schema().render());
    let mode = bless();
    if mode != Bless::No {
        refuse_unrequested_break(mode);
        write(SNAPSHOT, &expected);
        return;
    }
    assert!(
        read(SNAPSHOT) == expected,
        "{SNAPSHOT} does not list what the .proto files define; {BLESS_HINT}"
    );
}

#[test]
fn generated_code_is_current() {
    let expected = generate();
    let mode = bless();
    if mode != Bless::No {
        refuse_unrequested_break(mode);
        write(GENERATED, expected);
        return;
    }
    assert!(
        read(GENERATED) == expected,
        "{GENERATED} is not what prost-build generates from the .proto files; {BLESS_HINT}"
    );
}

#[test]
fn snapshot_text_round_trips() {
    let schema = current_schema();
    let parsed = Schema::parse(&schema.render()).expect("the rendered schema parses");
    assert_eq!(&parsed, schema);
}

// ---------------------------------------------------------------------------
// Rules of the schema that the snapshot does not express.
// ---------------------------------------------------------------------------

#[test]
fn every_file_is_proto3_in_the_one_package() {
    for file in &descriptors().file {
        assert_eq!(file.package(), PACKAGE, "{}", file.name());
        assert_eq!(file.syntax(), "proto3", "{}", file.name());
    }
}

#[test]
fn every_enum_starts_at_zero_with_an_unspecified_value() {
    for (name, item) in &current_schema().items {
        let Item::Enum(definition) = item else {
            continue;
        };
        let zero = definition
            .values
            .get(&0)
            .unwrap_or_else(|| panic!("{name} has no value 0"));
        // `StatusCode` follows the convention of status codes instead: 0 is OK,
        // so that a response without a status is a success.
        let expected = if name == "enum parties.v2.StatusCode" {
            "STATUS_CODE_OK"
        } else {
            "_UNSPECIFIED"
        };
        assert!(zero.ends_with(expected), "{name}: value 0 is {zero}");
    }
}

/// The field-number ranges of the areas in the envelope's `oneof`s, and the
/// file that defines each area's messages.
const AREAS: &[(&str, u32, u32)] = &[
    ("parties/v2/session.proto", 16, 31),
    ("parties/v2/voice.proto", 32, 63),
    ("parties/v2/structure.proto", 128, 159),
    ("parties/v2/roles.proto", 160, 191),
    ("parties/v2/server.proto", 192, 223),
];

/// The ranges kept free for the areas of a later pass: Chat, Moderation and
/// Bulk.
const RESERVED_FOR_LATER: &[(u32, u32)] = &[(64, 95), (96, 127), (224, 255)];

/// The file that defines each message, by full name.
fn defining_files() -> BTreeMap<String, String> {
    let mut files = BTreeMap::new();
    for file in &descriptors().file {
        for message in &file.message_type {
            files.insert(
                format!("message {PACKAGE}.{}", message.name()),
                file.name().to_owned(),
            );
        }
    }
    files
}

/// The members of `oneof` in `message`: number to (name, type).
fn oneof_members(message: &str, oneof: &str) -> BTreeMap<u32, (String, String)> {
    let Some(Item::Message(definition)) = current_schema().items.get(message) else {
        panic!("{message} is not a message of the schema");
    };
    let members: BTreeMap<_, _> = definition
        .fields
        .iter()
        .filter(|(_, field)| field.oneof.as_deref() == Some(oneof))
        .map(|(number, field)| (*number, (field.name.clone(), field.ty.clone())))
        .collect();
    assert!(!members.is_empty(), "{message} has no oneof {oneof}");
    members
}

#[test]
fn oneof_numbers_follow_the_area_ranges() {
    let files = defining_files();
    for (message, oneof) in [
        ("message parties.v2.ClientFrame", "body"),
        ("message parties.v2.Response", "payload"),
        ("message parties.v2.Event", "body"),
    ] {
        for (number, (name, ty)) in oneof_members(message, oneof) {
            let file = files
                .get(&ty)
                .unwrap_or_else(|| panic!("{message}.{name}: {ty} is not a top-level message"));
            let (_, first, last) = AREAS
                .iter()
                .find(|(area_file, _, _)| area_file == file)
                .unwrap_or_else(|| panic!("{message}.{name}: {file} is not the file of an area"));
            assert!(
                (*first..=*last).contains(&number),
                "{message}.{name} = {number} is outside {first} to {last}, the range of {file}"
            );
            for (reserved_first, reserved_last) in RESERVED_FOR_LATER {
                assert!(!(*reserved_first..=*reserved_last).contains(&number));
            }
        }
    }
}

#[test]
fn a_result_has_the_number_and_name_of_its_request() {
    let requests = oneof_members("message parties.v2.ClientFrame", "body");
    for (number, (name, _)) in oneof_members("message parties.v2.Response", "payload") {
        let (request, _) = requests.get(&number).unwrap_or_else(|| {
            panic!("Response.{name} = {number} has no request with that number")
        });
        assert_eq!(request, &name, "Response.payload field {number}");
    }
}

#[test]
fn the_envelope_keeps_its_first_fields_for_itself() {
    for (message, oneof) in [
        ("message parties.v2.ClientFrame", "body"),
        ("message parties.v2.Response", "payload"),
        ("message parties.v2.Event", "body"),
    ] {
        for (number, (name, _)) in oneof_members(message, oneof) {
            assert!(number >= 16, "{message}.{name} = {number}");
        }
    }
}

/// The requests of `docs/rust-rewrite/03-protocol-v2.md` section 5.4 that the
/// schema defines so far, as the names of their fields in `ClientFrame`.
const REQUESTS: &[&str] = &[
    // Session
    "update_profile",
    "set_presence",
    "link_report",
    "list_keys",
    "link_key",
    "unlink_key",
    // Voice
    "join_voice",
    "leave_voice",
    "set_voice_state",
    "publish_track",
    "update_track",
    "unpublish_track",
    "subscribe",
    "unsubscribe",
    // Structure
    "create_channel",
    "update_channel",
    "delete_channel",
    "reorder_channels",
    "set_overwrite",
    "delete_overwrite",
    "sync_channel_permissions",
    // Roles
    "create_role",
    "update_role",
    "delete_role",
    "reorder_roles",
    "assign_role",
    "revoke_role",
    // Server: settings and join codes
    "update_server_settings",
    "create_join_code",
    "revoke_join_code",
];

/// The events of section 5.5 that the schema defines so far.
const EVENTS: &[&str] = &[
    "server_updated",
    "channel_created",
    "channel_updated",
    "channel_deleted",
    "role_created",
    "role_updated",
    "role_deleted",
    "member_joined",
    "member_updated",
    "member_left",
    "presence_updated",
    "voice_state_updated",
    "track_published",
    "track_updated",
    "track_unpublished",
    "subscriber_changed",
    "permissions_updated",
    "join_code_changed",
];

#[test]
fn the_catalogue_of_the_document_is_in_the_envelope() {
    for (message, oneof, expected) in [
        ("message parties.v2.ClientFrame", "body", REQUESTS),
        ("message parties.v2.Event", "body", EVENTS),
    ] {
        let defined: Vec<String> = oneof_members(message, oneof)
            .into_values()
            .map(|(name, _)| name)
            .collect();
        for name in expected {
            assert!(
                defined.iter().any(|defined| defined == name),
                "{message} lacks {name}"
            );
        }
        // A request or an event added to the schema is added to the list
        // above, and to the document.
        for name in &defined {
            assert!(
                expected.contains(&name.as_str()),
                "{message}.{name} is not listed"
            );
        }
    }
}

#[test]
fn an_update_of_the_settings_uses_the_numbers_of_the_settings() {
    let fields = |message: &str| {
        let Some(Item::Message(definition)) = current_schema().items.get(message) else {
            panic!("{message} is not a message of the schema");
        };
        definition.fields.clone()
    };
    let settings = fields("message parties.v2.ServerSettings");
    let update = fields("message parties.v2.UpdateServerSettings");
    for (number, field) in &settings {
        // Two fields have a counterpart under another name: the update names
        // the revision it is based on, and it carries the verifier of a new
        // password where the settings only say that one is set.
        let counterpart = match field.name.as_str() {
            "revision" => "base_revision",
            "join_password_set" => "join_password",
            name => name,
        };
        let updated = update
            .get(number)
            .unwrap_or_else(|| panic!("UpdateServerSettings has no field {number}"));
        assert_eq!(updated.name, counterpart, "field {number}");
    }
    assert_eq!(update.len(), settings.len());
}

// ---------------------------------------------------------------------------
// The breaking-change check itself, on a small hand-written schema.
// ---------------------------------------------------------------------------

const BASE: &str = "\
enum t.Color
  0 COLOR_UNSPECIFIED
  1 COLOR_RED

message t.Thing
  1 id: uint64
  2 name: string
  3 tags: repeated string
  4 color: enum t.Color
  5 left: uint32 (oneof side)
  6 right: message t.Thing (oneof side)
  7 note: optional string
  reserved 9 to 11
  reserved name old_name
";

/// The breaking changes when `BASE` is the snapshot and `changed` the new
/// schema.
fn check(changed: &str) -> Vec<String> {
    let old = Schema::parse(BASE).expect("BASE parses");
    let new = Schema::parse(changed).expect("the changed schema parses");
    breaking_changes(&old, &new)
}

/// `BASE` with one line replaced, or removed when `with` is empty.
fn base_with(line: &str, with: &str) -> String {
    assert!(BASE.contains(line), "BASE has no line {line:?}");
    if with.is_empty() {
        BASE.replace(&format!("{line}\n"), "")
    } else {
        BASE.replace(line, with)
    }
}

#[track_caller]
fn assert_breaking(changed: &str, expected: &str) {
    let found = check(changed);
    assert_eq!(found.len(), 1, "{found:?}");
    assert!(
        found[0].contains(expected),
        "{found:?} does not mention {expected:?}"
    );
}

#[test]
fn an_unchanged_schema_is_compatible() {
    assert_eq!(check(BASE), Vec::<String>::new());
}

#[test]
fn additions_are_compatible() {
    let added = format!(
        "{}{}",
        base_with(
            "  7 note: optional string",
            "  7 note: optional string\n  8 extra: bytes\n  12 third: bool (oneof side)"
        )
        .replace("  1 COLOR_RED", "  1 COLOR_RED\n  2 COLOR_GREEN")
        .replace(
            "  reserved 9 to 11",
            "  reserved 9 to 11\n  reserved 20 to 29"
        ),
        "\nmessage t.Other\n  1 thing: message t.Thing\n\nenum t.Shape\n  0 SHAPE_UNSPECIFIED\n"
    );
    assert_eq!(check(&added), Vec::<String>::new());
}

#[test]
fn removing_a_field_is_breaking() {
    assert_breaking(
        &base_with("  2 name: string", ""),
        "t.Thing.name = 2 was removed",
    );
}

#[test]
fn removing_a_field_is_breaking_even_when_it_is_reserved() {
    let changed = base_with("  2 name: string", "")
        .replace("  reserved 9 to 11", "  reserved 2\n  reserved 9 to 11")
        .replace(
            "  reserved name old_name",
            "  reserved name name\n  reserved name old_name",
        );
    assert_breaking(&changed, "t.Thing.name = 2 was removed");
}

#[test]
fn renumbering_a_field_is_breaking() {
    assert_breaking(
        &base_with("  2 name: string", "  8 name: string"),
        "t.Thing.name was renumbered from 2 to 8",
    );
}

#[test]
fn renaming_a_field_is_breaking() {
    assert_breaking(
        &base_with("  2 name: string", "  2 title: string"),
        "t.Thing field 2 was renamed from name to title",
    );
}

#[test]
fn retyping_a_field_is_breaking() {
    assert_breaking(
        &base_with("  1 id: uint64", "  1 id: uint32"),
        "t.Thing.id = 1 changed type from uint64 to uint32",
    );
    assert_breaking(
        &base_with("  4 color: enum t.Color", "  4 color: int32"),
        "t.Thing.color = 4 changed type from enum t.Color to int32",
    );
}

#[test]
fn changing_a_label_is_breaking() {
    assert_breaking(
        &base_with("  3 tags: repeated string", "  3 tags: string"),
        "t.Thing.tags = 3 changed from repeated to singular",
    );
    assert_breaking(
        &base_with("  7 note: optional string", "  7 note: string"),
        "t.Thing.note = 7 changed from optional to singular",
    );
}

#[test]
fn moving_a_field_out_of_its_oneof_is_breaking() {
    assert_breaking(
        &base_with("  5 left: uint32 (oneof side)", "  5 left: uint32"),
        "t.Thing.left = 5 left oneof side",
    );
    assert_breaking(
        &base_with("  1 id: uint64", "  1 id: uint64 (oneof side)"),
        "t.Thing.id = 1 joined oneof side",
    );
}

#[test]
fn removing_or_renumbering_an_enum_value_is_breaking() {
    assert_breaking(
        &base_with("  1 COLOR_RED", ""),
        "t.Color value COLOR_RED = 1 was removed",
    );
    assert_breaking(
        &base_with("  1 COLOR_RED", "  2 COLOR_RED"),
        "t.Color value COLOR_RED was renumbered from 1 to 2",
    );
    assert_breaking(
        &base_with("  1 COLOR_RED", "  1 COLOR_CRIMSON"),
        "t.Color value 1 was renamed from COLOR_RED to COLOR_CRIMSON",
    );
}

#[test]
fn removing_a_message_or_an_enum_is_breaking() {
    let without_enum: String = BASE
        .lines()
        .filter(|line| !line.contains("COLOR") && !line.starts_with("enum"))
        .map(|line| format!("{line}\n"))
        .collect::<String>()
        .replace("  4 color: enum t.Color", "  4 color: int32");
    let found = check(&without_enum);
    assert!(
        found
            .iter()
            .any(|change| change.contains("enum t.Color was removed")),
        "{found:?}"
    );

    let only_enum = "enum t.Color\n  0 COLOR_UNSPECIFIED\n  1 COLOR_RED\n";
    assert_breaking(only_enum, "message t.Thing was removed");
}

#[test]
fn dropping_a_reservation_is_breaking() {
    assert_breaking(
        &base_with("  reserved 9 to 11", "  reserved 9 to 10"),
        "t.Thing no longer reserves 9 to 11",
    );
    assert_breaking(
        &base_with("  reserved name old_name", ""),
        "t.Thing no longer reserves the name old_name",
    );
    // Splitting or widening a range keeps every number reserved.
    let split = base_with("  reserved 9 to 11", "  reserved 9\n  reserved 10 to 15");
    assert_eq!(check(&split), Vec::<String>::new());
}

#[test]
fn every_breaking_change_is_reported() {
    let changed = "\
enum t.Color
  0 COLOR_UNSPECIFIED

message t.Thing
  1 id: string
  3 tags: repeated string
  4 color: enum t.Color
  5 left: uint32 (oneof side)
  6 right: message t.Thing (oneof side)
  8 note: optional string
";
    assert_eq!(check(changed).len(), 6, "{:#?}", check(changed));
}
