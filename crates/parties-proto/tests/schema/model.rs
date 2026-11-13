//! A plain model of a protobuf schema, its text form (the checked-in
//! snapshot) and the breaking-change check between two versions of it.
//!
//! The text form is one block per message or enum, sorted by full name:
//!
//! ```text
//! enum parties.v2.ChannelKind
//!   0 CHANNEL_KIND_UNSPECIFIED
//!   1 CHANNEL_KIND_CATEGORY
//!
//! message parties.v2.Overwrite
//!   1 role_id: uint64 (oneof target)
//!   2 user_id: uint64 (oneof target)
//!   3 allow: uint64
//!   8 overwrites: repeated message parties.v2.Overwrite
//!   9 name: optional string
//!   reserved 12 to 15
//!   reserved name old_name
//! ```
//!
//! Lines that start with `#` and empty lines are ignored.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use prost_types::field_descriptor_proto::{Label, Type};
use prost_types::{DescriptorProto, EnumDescriptorProto, FieldDescriptorProto, FileDescriptorSet};

/// Every message and enum of a schema, by kind and full name, for example
/// `"message parties.v2.Role"`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Schema {
    pub items: BTreeMap<String, Item>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Item {
    Message(MessageDef),
    Enum(EnumDef),
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MessageDef {
    /// By field number.
    pub fields: BTreeMap<u32, FieldDef>,
    pub reserved: Reserved,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EnumDef {
    /// By number.
    pub values: BTreeMap<i32, String>,
    pub reserved: Reserved,
}

/// The reserved numbers, as inclusive ranges, and the reserved names.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Reserved {
    pub ranges: BTreeSet<(i64, i64)>,
    pub names: BTreeSet<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cardinality {
    Singular,
    /// Proto3 `optional`: the field records whether it was set.
    Optional,
    Repeated,
}

impl Cardinality {
    fn name(self) -> &'static str {
        match self {
            Self::Singular => "singular",
            Self::Optional => "optional",
            Self::Repeated => "repeated",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FieldDef {
    pub name: String,
    pub cardinality: Cardinality,
    /// A scalar type name, `message <full name>` or `enum <full name>`.
    pub ty: String,
    pub oneof: Option<String>,
}

// ---------------------------------------------------------------------------
// From compiled descriptors
// ---------------------------------------------------------------------------

fn scalar_name(ty: Type) -> &'static str {
    match ty {
        Type::Double => "double",
        Type::Float => "float",
        Type::Int64 => "int64",
        Type::Uint64 => "uint64",
        Type::Int32 => "int32",
        Type::Fixed64 => "fixed64",
        Type::Fixed32 => "fixed32",
        Type::Bool => "bool",
        Type::String => "string",
        Type::Bytes => "bytes",
        Type::Uint32 => "uint32",
        Type::Sfixed32 => "sfixed32",
        Type::Sfixed64 => "sfixed64",
        Type::Sint32 => "sint32",
        Type::Sint64 => "sint64",
        Type::Group | Type::Message | Type::Enum => unreachable!("not a scalar"),
    }
}

fn field_def(message: &DescriptorProto, field: &FieldDescriptorProto) -> FieldDef {
    let type_name = field.type_name().trim_start_matches('.');
    let ty = match field.r#type() {
        Type::Message => format!("message {type_name}"),
        Type::Enum => format!("enum {type_name}"),
        Type::Group => panic!("{}: groups are not used", field.name()),
        scalar => scalar_name(scalar).to_owned(),
    };
    let optional = field.proto3_optional();
    let cardinality = if field.label() == Label::Repeated {
        Cardinality::Repeated
    } else if optional {
        Cardinality::Optional
    } else {
        Cardinality::Singular
    };
    // A proto3 `optional` field sits in a oneof the compiler made up for it.
    let oneof = match field.oneof_index {
        Some(index) if !optional => {
            let index = usize::try_from(index).expect("oneof index");
            Some(message.oneof_decl[index].name().to_owned())
        }
        _ => None,
    };
    FieldDef {
        name: field.name().to_owned(),
        cardinality,
        ty,
        oneof,
    }
}

impl Schema {
    /// The schema of every file in `set`.
    pub fn from_descriptors(set: &FileDescriptorSet) -> Self {
        let mut schema = Self::default();
        for file in &set.file {
            let package = file.package();
            for message in &file.message_type {
                schema.add_message(package, message);
            }
            for definition in &file.enum_type {
                schema.add_enum(package, definition);
            }
        }
        schema
    }

    fn insert(&mut self, key: String, item: Item) {
        let previous = self.items.insert(key.clone(), item);
        assert!(previous.is_none(), "{key} is defined twice");
    }

    fn add_message(&mut self, scope: &str, message: &DescriptorProto) {
        let full_name = format!("{scope}.{}", message.name());
        let mut definition = MessageDef::default();
        for field in &message.field {
            let number = u32::try_from(field.number()).expect("field numbers are positive");
            let previous = definition.fields.insert(number, field_def(message, field));
            assert!(previous.is_none(), "{full_name}: field {number} twice");
        }
        for range in &message.reserved_range {
            // The end of a message's reserved range is exclusive.
            let range = (i64::from(range.start()), i64::from(range.end()) - 1);
            definition.reserved.ranges.insert(range);
        }
        definition.reserved.names = message.reserved_name.iter().cloned().collect();
        self.insert(format!("message {full_name}"), Item::Message(definition));
        for nested in &message.nested_type {
            self.add_message(&full_name, nested);
        }
        for nested in &message.enum_type {
            self.add_enum(&full_name, nested);
        }
    }

    fn add_enum(&mut self, scope: &str, definition: &EnumDescriptorProto) {
        let full_name = format!("{scope}.{}", definition.name());
        let mut item = EnumDef::default();
        for value in &definition.value {
            let previous = item.values.insert(value.number(), value.name().to_owned());
            assert!(previous.is_none(), "{full_name}: aliases are not used");
        }
        for range in &definition.reserved_range {
            // The end of an enum's reserved range is inclusive.
            let range = (i64::from(range.start()), i64::from(range.end()));
            item.reserved.ranges.insert(range);
        }
        item.reserved.names = definition.reserved_name.iter().cloned().collect();
        self.insert(format!("enum {full_name}"), Item::Enum(item));
    }
}

// ---------------------------------------------------------------------------
// Text form
// ---------------------------------------------------------------------------

fn render_reserved(out: &mut String, reserved: &Reserved) {
    for &(first, last) in &reserved.ranges {
        if first == last {
            writeln!(out, "  reserved {first}").expect("writing to a string");
        } else {
            writeln!(out, "  reserved {first} to {last}").expect("writing to a string");
        }
    }
    for name in &reserved.names {
        writeln!(out, "  reserved name {name}").expect("writing to a string");
    }
}

fn parse_reserved(words: &[&str], reserved: &mut Reserved) -> Result<(), String> {
    let number = |word: &str| {
        word.parse::<i64>()
            .map_err(|_| format!("'{word}' is not a number"))
    };
    match words {
        ["name", name] => {
            reserved.names.insert((*name).to_owned());
        }
        [single] => {
            let single = number(single)?;
            reserved.ranges.insert((single, single));
        }
        [first, "to", last] => {
            reserved.ranges.insert((number(first)?, number(last)?));
        }
        _ => {
            return Err("expected 'reserved N', 'reserved N to M' or 'reserved name X'".to_owned());
        }
    }
    Ok(())
}

fn parse_field(words: &[&str]) -> Result<(u32, FieldDef), String> {
    let [number, name, rest @ ..] = words else {
        return Err("expected '<number> <name>: <type>'".to_owned());
    };
    let number = number
        .parse::<u32>()
        .map_err(|_| format!("'{number}' is not a field number"))?;
    let name = name
        .strip_suffix(':')
        .ok_or_else(|| format!("expected ':' after the field name '{name}'"))?;
    let (cardinality, rest) = match rest {
        ["repeated", rest @ ..] => (Cardinality::Repeated, rest),
        ["optional", rest @ ..] => (Cardinality::Optional, rest),
        rest => (Cardinality::Singular, rest),
    };
    let (ty, rest) = match rest {
        [kind @ ("message" | "enum"), type_name, rest @ ..] => {
            (format!("{kind} {type_name}"), rest)
        }
        [scalar, rest @ ..] => ((*scalar).to_owned(), rest),
        [] => return Err(format!("field '{name}' has no type")),
    };
    let oneof = match rest {
        [] => None,
        ["(oneof", oneof] => Some(
            oneof
                .strip_suffix(')')
                .ok_or_else(|| "expected ')' after the oneof name".to_owned())?
                .to_owned(),
        ),
        _ => return Err(format!("unexpected text after the type of '{name}'")),
    };
    let field = FieldDef {
        name: name.to_owned(),
        cardinality,
        ty,
        oneof,
    };
    Ok((number, field))
}

impl Schema {
    /// The text form. [`Schema::parse`] reads it back.
    pub fn render(&self) -> String {
        let mut out = String::new();
        for (index, (key, item)) in self.items.iter().enumerate() {
            if index > 0 {
                out.push('\n');
            }
            writeln!(out, "{key}").expect("writing to a string");
            match item {
                Item::Message(message) => {
                    for (number, field) in &message.fields {
                        write!(out, "  {number} {}: ", field.name).expect("writing to a string");
                        if field.cardinality != Cardinality::Singular {
                            write!(out, "{} ", field.cardinality.name())
                                .expect("writing to a string");
                        }
                        out.push_str(&field.ty);
                        if let Some(oneof) = &field.oneof {
                            write!(out, " (oneof {oneof})").expect("writing to a string");
                        }
                        out.push('\n');
                    }
                    render_reserved(&mut out, &message.reserved);
                }
                Item::Enum(definition) => {
                    for (number, name) in &definition.values {
                        writeln!(out, "  {number} {name}").expect("writing to a string");
                    }
                    render_reserved(&mut out, &definition.reserved);
                }
            }
        }
        out
    }

    /// Reads the text form. The error names the line.
    pub fn parse(text: &str) -> Result<Self, String> {
        let mut schema = Self::default();
        let mut current: Option<(String, Item)> = None;
        for (index, line) in text.lines().enumerate() {
            let fail = |message: String| format!("line {}: {message}", index + 1);
            if line.trim().is_empty() || line.starts_with('#') {
                continue;
            }
            let words: Vec<&str> = line.split_whitespace().collect();
            if !line.starts_with(' ') {
                if let Some((key, item)) = current.take() {
                    schema.insert(key, item);
                }
                let item = match words.as_slice() {
                    ["message", _] => Item::Message(MessageDef::default()),
                    ["enum", _] => Item::Enum(EnumDef::default()),
                    _ => {
                        return Err(fail(
                            "expected 'message <name>' or 'enum <name>'".to_owned(),
                        ));
                    }
                };
                current = Some((words.join(" "), item));
                continue;
            }
            let Some((_, item)) = current.as_mut() else {
                return Err(fail(
                    "a member line before the first message or enum".to_owned(),
                ));
            };
            match (item, words.as_slice()) {
                (Item::Message(message), ["reserved", rest @ ..]) => {
                    parse_reserved(rest, &mut message.reserved).map_err(fail)?;
                }
                (Item::Enum(definition), ["reserved", rest @ ..]) => {
                    parse_reserved(rest, &mut definition.reserved).map_err(fail)?;
                }
                (Item::Message(message), words) => {
                    let (number, field) = parse_field(words).map_err(fail)?;
                    if message.fields.insert(number, field).is_some() {
                        return Err(fail(format!("field {number} is listed twice")));
                    }
                }
                (Item::Enum(definition), [number, name]) => {
                    let number = number
                        .parse::<i32>()
                        .map_err(|_| fail(format!("'{number}' is not an enum number")))?;
                    if definition
                        .values
                        .insert(number, (*name).to_owned())
                        .is_some()
                    {
                        return Err(fail(format!("value {number} is listed twice")));
                    }
                }
                (Item::Enum(_), _) => return Err(fail("expected '<number> <NAME>'".to_owned())),
            }
        }
        if let Some((key, item)) = current {
            schema.insert(key, item);
        }
        Ok(schema)
    }
}

// ---------------------------------------------------------------------------
// The breaking-change check
// ---------------------------------------------------------------------------

/// Whether every number of `range` lies in one of the ranges of `by`.
fn covered(range: (i64, i64), by: &BTreeSet<(i64, i64)>) -> bool {
    let (mut next, last) = range;
    // The set is ordered by the start of each range.
    for &(start, end) in by {
        if start > next {
            break;
        }
        if end >= next {
            next = end + 1;
            if next > last {
                return true;
            }
        }
    }
    next > last
}

fn reserved_changes(what: &str, old: &Reserved, new: &Reserved, changes: &mut Vec<String>) {
    for &(first, last) in &old.ranges {
        if !covered((first, last), &new.ranges) {
            let range = if first == last {
                first.to_string()
            } else {
                format!("{first} to {last}")
            };
            changes.push(format!("{what} no longer reserves {range}"));
        }
    }
    for name in &old.names {
        if !new.names.contains(name) {
            changes.push(format!("{what} no longer reserves the name {name}"));
        }
    }
}

fn message_changes(name: &str, old: &MessageDef, new: &MessageDef, changes: &mut Vec<String>) {
    for (number, before) in &old.fields {
        let field = format!("{name}.{} = {number}", before.name);
        let Some(after) = new.fields.get(number) else {
            let moved = new
                .fields
                .iter()
                .find(|(_, field)| field.name == before.name);
            changes.push(match moved {
                Some((to, _)) => {
                    format!(
                        "{name}.{} was renumbered from {number} to {to}",
                        before.name
                    )
                }
                None => format!("{field} was removed"),
            });
            continue;
        };
        if after.name != before.name {
            // A field that took the number of another one is reported once,
            // as the renumbering or removal of the field that had it.
            if new.fields.values().any(|field| field.name == before.name) {
                let to = new
                    .fields
                    .iter()
                    .find(|(_, field)| field.name == before.name)
                    .map(|(to, _)| *to)
                    .expect("found above");
                changes.push(format!(
                    "{name}.{} was renumbered from {number} to {to}",
                    before.name
                ));
            } else {
                changes.push(format!(
                    "{name} field {number} was renamed from {} to {}",
                    before.name, after.name
                ));
            }
            continue;
        }
        if after.ty != before.ty {
            changes.push(format!(
                "{field} changed type from {} to {}",
                before.ty, after.ty
            ));
        }
        if after.cardinality != before.cardinality {
            changes.push(format!(
                "{field} changed from {} to {}",
                before.cardinality.name(),
                after.cardinality.name()
            ));
        }
        match (&before.oneof, &after.oneof) {
            (Some(left), Some(joined)) if left != joined => {
                changes.push(format!("{field} moved from oneof {left} to oneof {joined}"));
            }
            (Some(left), None) => changes.push(format!("{field} left oneof {left}")),
            (None, Some(joined)) => changes.push(format!("{field} joined oneof {joined}")),
            _ => {}
        }
    }
    reserved_changes(name, &old.reserved, &new.reserved, changes);
}

fn enum_changes(name: &str, old: &EnumDef, new: &EnumDef, changes: &mut Vec<String>) {
    for (number, before) in &old.values {
        match new.values.get(number) {
            Some(after) if after == before => {}
            other => {
                let moved = new.values.iter().find(|(_, value)| *value == before);
                changes.push(match (moved, other) {
                    (Some((to, _)), _) => {
                        format!("{name} value {before} was renumbered from {number} to {to}")
                    }
                    (None, Some(after)) => {
                        format!("{name} value {number} was renamed from {before} to {after}")
                    }
                    (None, None) => format!("{name} value {before} = {number} was removed"),
                });
            }
        }
    }
    reserved_changes(name, &old.reserved, &new.reserved, changes);
}

/// What `new` breaks for a peer built from `old`: a message, an enum, a
/// field, a value or a reservation of `old` that `new` no longer has in
/// exactly that form. Additions are not listed. Empty when `new` is
/// compatible.
pub fn breaking_changes(old: &Schema, new: &Schema) -> Vec<String> {
    let mut changes = Vec::new();
    for (key, before) in &old.items {
        let name = key.split(' ').nth(1).unwrap_or(key);
        match (before, new.items.get(key)) {
            (_, None) => changes.push(format!("{key} was removed")),
            (Item::Message(before), Some(Item::Message(after))) => {
                message_changes(name, before, after, &mut changes);
            }
            (Item::Enum(before), Some(Item::Enum(after))) => {
                enum_changes(name, before, after, &mut changes);
            }
            (Item::Message(_), Some(Item::Enum(_))) | (Item::Enum(_), Some(Item::Message(_))) => {
                unreachable!("the key names the kind")
            }
        }
    }
    changes
}
