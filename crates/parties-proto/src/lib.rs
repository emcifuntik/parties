//! The wire schema of Parties protocol 2.0.
//!
//! This crate makes `docs/rust-rewrite/03-protocol-v2.md` normative: the
//! control messages, the framing, the media headers, and every limit and
//! code, in one place that the server, the client and the admin CLI share.
//! It does no I/O and depends on no other crate of the workspace.
//!
//! | Module | Section of the document | What it holds |
//! |---|---|---|
//! | [`v2`] | 4, 5 | The protobuf messages of package `parties.v2`, generated |
//! | [`frame`] | 3 | `[length varint][protobuf message]` framing with a per-stream limit |
//! | [`snapshot`] | 5.3 | Putting a `Ready` snapshot sent in several frames back together |
//! | [`stream`] | 2 | Stream types, and what an unknown type means |
//! | [`media`] | 6 | Datagram layouts and the header of a video frame stream |
//! | [`varint`] | | QUIC variable-length integers (RFC 9000 section 16) |
//! | [`ids`] | 5.2 | The 64-bit time-sortable identifier, session and track ids |
//! | [`limits`] | 9 | Every bound as a named constant, and the rate classes |
//! | [`codes`] | 10 | Connection close codes and stream reset codes |
//! | [`version`] | 1, 11 | ALPN token, protocol minor version, feature negotiation |
//!
//! Signatures, transcripts, the TLS exporter and the encodings of keys are
//! not here: `parties-identity` owns everything cryptographic. This crate
//! only names the fields that carry them.
//!
//! # A connection, in the types of this crate
//!
//! 1. QUIC and TLS 1.3 with the ALPN token [`version::ALPN`].
//! 2. The client opens the control stream: it writes
//!    [`stream::StreamType::Control`], then frames with
//!    [`frame::FrameCodec::CONTROL_PRE_AUTH`].
//! 3. Handshake: the client sends [`v2::ClientHandshake`] frames (`Hello`,
//!    `Authenticate`), the server answers with [`v2::ServerHandshake`]
//!    frames (`HelloAck`, `AuthResult`, then `Ready`, possibly in several
//!    frames: [`snapshot::ReadyAssembler`]).
//! 4. From then on the limit is [`frame::FrameCodec::CONTROL`], the client
//!    sends [`v2::ClientFrame`] and the server [`v2::ServerFrame`].
//! 5. Audio travels in datagrams ([`media::datagram`]), each video frame on
//!    a unidirectional stream of its own ([`media::video`]).
//! 6. A close carries a [`codes::CloseCode`], a stream reset a
//!    [`codes::StreamResetCode`].
//!
//! # Untrusted input
//!
//! Every decoder here takes bytes from the network. None of them panics,
//! whatever the input, and none allocates for a length it has not checked:
//! each returns a typed error or asks for more bytes. The property tests in
//! `tests/properties.rs` hold them to that.
//!
//! # Generated code
//!
//! The messages are defined in `proto/parties/v2/*.proto` and generated
//! with `prost`. The generated file, `src/generated/parties.v2.rs`, is
//! checked in, and there is no build script. `tests/schema.rs` compiles the
//! `.proto` files with a protobuf compiler written in Rust (`protox`),
//! regenerates the code in a temporary directory and fails when the
//! checked-in file is stale.
//!
//! The alternative was to generate in `build.rs`. It was not chosen
//! because:
//!
//! - Building this crate, and so every crate above it, needs only `prost`,
//!   `bytes` and `thiserror`: no `protoc` binary, and no compiler or code
//!   generator among the build dependencies. That keeps builds short and
//!   cross-compiling to iOS exactly as easy as for any pure Rust crate.
//! - A change of the schema shows up in review as a change of the code that
//!   consumers compile against.
//! - The generator's output is pinned by the checked-in file, so an update
//!   of `prost-build` cannot silently change the types.
//!
//! After a change to a `.proto` file, regenerate the code and the schema
//! snapshot:
//!
//! ```text
//! PARTIES_PROTO_BLESS=1 cargo test -p parties-proto --test schema
//! ```
//!
//! (In PowerShell: `$env:PARTIES_PROTO_BLESS = '1'`, run the test, then
//! `Remove-Item Env:PARTIES_PROTO_BLESS`.) The test needs the library to
//! build, so regenerate first and adapt hand-written code to a renamed or
//! removed item afterwards.
//!
//! # Breaking-change check
//!
//! `schema/parties.v2.snapshot` lists every message, field name, number,
//! type and label, and every enum value. `tests/schema.rs` fails when the
//! `.proto` files remove, renumber, rename or retype anything the snapshot
//! lists, or drop a reservation. Additions pass once the snapshot is
//! regenerated with the command above; that command refuses to run over a
//! breaking change unless it is given `PARTIES_PROTO_BLESS=breaking`, which
//! is for deliberate breaks before the schema is frozen for 2.0.0.
//!
//! # Schema rules
//!
//! - Field numbers are never reused; a removed field is reserved.
//! - New fields are optional for the receiver; unknown fields are ignored.
//! - The `oneof`s of the envelope assign field numbers by area, in ranges
//!   documented in `proto/parties/v2/envelope.proto`. The ranges of Chat
//!   (64 to 95), Moderation (96 to 127) and Bulk (224 to 255) are kept free
//!   for a later pass.
//! - An enum value a receiver does not know is rejected by the code that
//!   validates the request, never stored
//!   (`docs/rust-rewrite/04-connection-security.md` section 7). The
//!   generated types keep enum fields as `i32` for that reason; use
//!   `TryFrom<i32>` or the generated accessor.
//!
//! # What the snapshot carries for `parties-perm`
//!
//! The state snapshot carries exactly what the permission engine models
//! (`docs/rust-rewrite/05-permissions-and-roles.md`), so a client can run
//! the same resolution the server runs. This crate does not depend on
//! `parties-perm`; the mapping is by shape:
//!
//! | `parties-perm` | On the wire |
//! |---|---|
//! | `UserId`, `RoleId`, `ChannelId`, `MessageId` | `uint64`; [`ids`] has newtypes of the same shape |
//! | `Permissions` | `uint64` masks with the same bit values; bit 63 is never set |
//! | `Timestamp` | `uint64` milliseconds since the Unix epoch (`*_at_ms`, `*_until_ms`); 0 for "none" |
//! | `Role { id, position, permissions, limits }` | [`v2::Role`], which adds name, colour, flags and revision |
//! | `RoleLimits` of a role or of a member | [`v2::RoleLimits`]: the same seven fields; absent means all zero |
//! | `RoleLimits` used as caps (`ServerView::limit_caps`, `ChannelView::limit_caps`) | [`v2::LimitCaps`]: a field that is not set is `u32::MAX`, no cap |
//! | `Overwrite` with `OverwriteTarget::Role` or `Member` | [`v2::Overwrite`] with `role_id` or `user_id`, `allow`, `deny` |
//! | `ChannelView { id, overwrites, limit_caps }` | [`v2::Channel`] |
//! | `MemberView { user_id, roles, joined_at, timeout_until }` | [`v2::Member`]: `user_id`, `role_ids`, `joined_at_ms`, `timeout_until_ms` |
//! | `MemberView::speak_grant` | The `channel_id` of the member's [`v2::VoiceState`] that has `speak_granted` |
//! | `NewMemberPolicy { period_ms, require_role, restricted }` | [`v2::NewMemberPolicy`], in [`v2::ServerSettings`] |
//! | `ServerView { owner_id, everyone, limit_caps, new_members }` | `Ready.owner_id`, `Ready.everyone_role_id`, `ServerSettings.limit_caps`, `ServerSettings.new_members` |
//! | `effective()`, `effective_limits()` for the receiving member | `server_permissions`, `channel_permissions` and `limits` of [`v2::Ready`] and [`v2::PermissionsUpdated`] |
//! | `Denied`, `Rule` | [`v2::Status`]: `missing_permissions` and `rule` ([`v2::HierarchyRule`]) |

#![warn(missing_docs)]

pub mod codes;
pub mod frame;
pub mod ids;
pub mod limits;
pub mod media;
pub mod snapshot;
pub mod stream;
pub mod varint;
pub mod version;

/// The protobuf messages of package `parties.v2`: the handshake, the
/// envelope, the state types of the `Ready` snapshot, and the requests and
/// events of the areas Session, Voice, Structure, Roles and Server.
///
/// Generated from `proto/parties/v2/*.proto`; see the crate documentation.
/// The comments on the items come from the `.proto` files, where "section"
/// refers to `docs/rust-rewrite/03-protocol-v2.md`.
#[allow(missing_docs, clippy::all)]
pub mod v2 {
    include!("generated/parties.v2.rs");
}

mod convert;

// The crates whose types appear in this crate's API, so that a consumer
// uses the same versions without naming them.
pub use bytes;
pub use prost;

pub use codes::{CloseCode, StreamResetCode};
pub use frame::{FrameCodec, FrameError};
pub use ids::{
    AttachmentId, AuditEntryId, ChannelId, JoinCodeId, MessageId, RoleId, SessionId,
    TimeSortableId, TrackId, UserId,
};
pub use stream::StreamType;
pub use varint::VarInt;
