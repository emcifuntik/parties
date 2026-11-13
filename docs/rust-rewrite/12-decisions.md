# 12 — Decision log and open questions

> Status: Draft — 2026-11-10. Decisions are **proposed** unless marked
> **Decided by the owner**. Decisions marked "gated" are settled by a Phase 0
> spike.

## 1. Decisions

### D-01 — Migration strategy
**Proposed:** build the Rust system in parallel, freeze the C++ line, cut over
once. Protocol 2.0 is not wire-compatible with 1.4.
**Alternatives:** incremental replacement inside the C++ app; a Rust server
that first speaks 1.4.
**Why:** the authentication flaw (F1) and the role model cannot be fixed
compatibly, so any path through 1.4 ports work that is then discarded.
See [10](10-migration-plan.md) section 1.

### D-02 — QUIC stack (gated: transport spike)
**Proposed:** `quinn` with `rustls`, hidden behind `parties-transport`.
**Alternatives:** the official MsQuic Rust bindings (keeps today's tuning and
patches, keeps a C dependency and the patch maintenance); `quiche`;
`s2n-quic`.
**Why:** pure Rust on every target including iOS; a bounded drop-oldest
datagram buffer that the 1.3 work had to hand-build on MsQuic; a custom
socket hook that replaces patch 0001; a pluggable congestion controller; a
TLS exporter for channel binding.
**Risk:** congestion behaviour must be re-validated (R-01). MsQuic stays the
fallback.

### D-03 — Control-plane encoding
**Proposed:** Protocol Buffers with `prost`; fixed binary headers for media.
**Alternatives:** keep hand-written serialization; `postcard` or `bincode`
(compact but Rust-only, which shuts out third-party clients and bots);
FlatBuffers or Cap'n Proto.
**Why:** schema evolution rules that tooling can check, and a format other
languages can read.

### D-04 — UI toolkit
**Decided by the owner (2026-11-10):** [lurq](https://github.com/ivanzaida/lurq),
a Rust toolkit with typed component builders, reactive signals and
`wgpu`/DX12 renderers (MIT).
**Supersedes:** the earlier proposal of Slint with egui as fallback.
**Consequences:** the application needs four things lurq's guide does not
document today; they are work items, listed in
[08](08-client-design.md) section 6.1: importing GPU textures the app already
owns (zero-copy video), a shell for iOS, an accessibility tree, and a shared
GPU device between the UI and the video pipeline. Tray, global hotkeys and
native menus on Windows stay outside the toolkit.
**First evidence (2026-11-12):** the chat window of the Glass design was built
on lurq 0.60 in `crates/parties-ui`. It matches the design picture closely
(panel edges on the same pixels, sampled colours within 2/255), costs 1.5 ms
of work per frame and draws nothing while idle. Backdrop blur, the one thing
the design needed and 0.60 lacked, was added to lurq's master the same day
and released in 0.61.0 on 2026-11-13, which the window builds against (from
crates.io, with no local patch). Gradient borders
and Windows snap layouts are still missing; details in
[08](08-client-design.md) section 6.1. The owner asked
how lurq compares with GPUI; the comparison was described but not run.

### D-05 — Async runtime
**Proposed:** `tokio`.

### D-06 — Server storage
**Proposed:** SQLite through `rusqlite`, one writer thread, a small reader
pool, embedded forward-only migrations.
**Alternatives:** PostgreSQL (operational burden for self-hosters);
`sqlx` (async and compile-time-checked queries, heavier build).

### D-07 — User identity
**Proposed:** Ed25519 identity from a recovery phrase with a versioned
derivation: v1 kept for import, standard BIP-39 for new identities. The root
key lives in the OS keystore. Device keys and per-server identities are
reserved in the protocol and built after launch.

### D-08 — Server trust
**Proposed:** a long-term server identity key, pinned by clients. Join
links carry the key. CA-issued certificates are optional. A new key is never
trusted silently, and a changed key is a hard failure unless a rotation
statement is presented. See [04](04-connection-security.md) section 4.

### D-09 — Joining a server
**Proposed:** join policies (open, password, join code, approval) decide
how an identity becomes a member; after that the key is enough. The
per-connection shared password goes away.

### D-10 — Permission model
**Proposed:** Discord's resolution algorithm, a strict role hierarchy with a
no-escalation rule, and numeric role limits. See
[05](05-permissions-and-roles.md).

### D-11 — End-to-end media encryption
**Proposed:** not in 2.0. The packet layout reserves what it needs. The
product must state plainly that the server operator can access media, and the
README claim is corrected now.
**Alternative:** make it a 2.0 requirement, at the cost of a later launch
(key agreement for groups is a project of its own).
**Needs from the owner:** priority (question Q6).

### D-12 — Video codecs: Rust wrappers, no bridge
**Decided by the owner (2026-11-10):** write Rust wrappers for AMD AMF and
for NVIDIA's Video Codec SDK (NVENC and NVDEC), and port the `encdec`
library to Rust on top of them. No C++ from this repository ships in a Rust
build.
**Supersedes:** the earlier proposal to link the existing `encdec` library
temporarily behind a feature flag.
**Consequences:** the vendor backends are on the critical path of the video
phase instead of being deferred. Structure and order are in
[08](08-client-design.md) section 5.4.

### D-13 — Repository layout
**Decided by the owner (2026-11-12):** the Cargo workspace lives at the root
of this repository next to the CMake tree until cutover. It was created that
day (`Cargo.toml`, `crates/`, `rust-toolchain.toml`, `.github/workflows/rust.yml`).
**Alternative:** a new repository (clean history, but the port loses direct
access to the code, tests and documents it is porting).

### D-14 — Sessions
**Proposed:** the protocol and core support several sessions per identity and
several server connections per client. The launch UI may expose one
connection at a time.

### D-15 — Identifiers
**Proposed:** 64-bit time-sortable ids on the control plane; 32-bit track ids
on the media plane.

### D-16 — Audio processing (gated: listening test in Phase 0–2)
**Proposed:** libopus; a Rust RNNoise port for suppression; WebRTC audio
processing for echo cancellation and gain on Windows and macOS; the system
voice-processing unit on iOS.
**Alternative:** keep speexdsp (simplest port, weakest canceller).

### D-17 — Update mechanism
**Proposed:** a manifest signed by a dedicated Ed25519 release key that is
compiled into the client, in addition to platform code signing. Sparkle stays
on macOS. The Windows installer and updater tooling is chosen in Phase 2.

### D-18 — Licensing
**Proposed:** the project stays MIT. Dependencies must be permissively
licensed unless an exception is recorded here. lurq is MIT. Known items to
clear: the WebRTC audio library, the redistribution terms of the AMF and
NVIDIA SDK headers the wrappers are generated from, and H.265, which is used
only through operating-system and hardware encoders and decoders.

### D-19 — Codec negotiation
**Proposed:** participants advertise what they can decode; the publisher
picks a codec everyone in the room supports, with H.264 as the floor. The
bundled software H.265 decoder is dropped.

### D-20 — No 0-RTT, no resumption in 2.0
**Proposed:** both off. Authentication never depends on TLS session state.

### D-21 — Performance is a requirement, not a goal
**Decided by the owner (2026-11-10):** the rewrite aims for "perfect
performance": zero-copy wherever possible, no leaks, minimal memory use.
**How it is made checkable:** copy counts per media path, memory ceilings per
scenario and leak gates in CI. See [02](02-target-architecture.md) section 10
and [11](11-quality-and-release.md) section 9.

### D-22 — Members are shown online and offline
**Decided by the owner (2026-11-10).** The client shows every member of a
server with their presence, not only the people currently in voice. Design in
[13](13-profiles-access-and-hardware-keys.md) section 2.

### D-23 — Profile pictures are synchronised by hash
**Decided by the owner (2026-11-10).** The picture is stored on the client.
On connect the client sends its SHA-512; the server asks for the bytes only
when the hash differs from what it holds. Design in
[13](13-profiles-access-and-hardware-keys.md) section 3.

### D-24 — Rename yourself by double-clicking your own name
**Decided by the owner (2026-11-10).** Design in
[13](13-profiles-access-and-hardware-keys.md) section 4.

### D-25 — Join codes
**Decided by the owner (2026-11-10).** Members with the right permission
create single-use codes. A new user who presents one becomes a member; the
server remembers their identity fingerprint and lets them in from then on.
This is the concrete form of the "invites" in the earlier drafts. Design in
[13](13-profiles-access-and-hardware-keys.md) section 5.

### D-26 — Optional hardware-backed identity (YubiKey)
**Decided by the owner (2026-11-10):** a user may authenticate with a key
held on a YubiKey instead of a key held in software.
**Proposed mechanism:** FIDO2 first, because it works with every current
YubiKey and with other security keys, with an account able to hold several
keys so that losing one is not losing the account. Design and open points in
[13](13-profiles-access-and-hardware-keys.md) section 6.

### D-27 — Ownership and removal are host operations
**Decided by the owner (2026-11-11):** servers are self-hosted, so deleting a
server and changing its owner are not possible outside the server's
configuration. The client has no "danger zone" and the protocol has no
`TransferOwnership` or delete-server request.
**How it is done instead:** the owner is set by `[owner] key` in `server.toml`
(or by `parties-server owner reset` on the host); a server is removed by
stopping it and deleting its data directory. See
[07](07-server-management.md) sections 2 and 8.
**Open:** whether the first-run claim link stays as the way the first owner is
set, or the config file becomes the only way.

### D-28 — The identity key can be kept in a passkey
**Decided by the owner (2026-11-11):** passkeys are supported, and the passkey
is where the key is kept ("we save our key in a passkey"). Optional, next to
"on this device" and "on a hardware key".
**Purpose, in the owner's words:** not security; it is there so the key does
not get lost.
**Mechanism (accepted by the owner, Q12):** the key is derived from the
passkey through the PRF extension, so the same passkey gives the same identity
on every device it is synchronised to, and the server and the protocol do not
change. An existing identity gets a passkey as a second, linked key.
**Unlocking (decided, Q13):** once per device; after that the key is also in
the OS keystore, like a software key. Design, platform support and risks in
[13](13-profiles-access-and-hardware-keys.md) section 7.
**Open:** the relying-party domain (Q9); a Phase 0 spike must show that the
derived key is identical across devices.

### D-29 — The user interface calls a server a "party"
**Decided by the owner (2026-11-11):** everywhere in the client a server is a
"party" (plural "parties"), not a "space". The protocol, the server and these
documents keep the word "server".

### D-30 — What the first version leaves out
**Decided by the owner (2026-11-12):** not everything that was drawn or
specified is built in the first version. Left out, and removed from the Glass
frames so that the design shows what is built:

| Left out | Meaning |
|---|---|
| Signed-in devices | No list of the user's sessions in the client and no remote sign-out |
| The "Global shortcuts" settings | The two toggles are gone. Shortcuts and push-to-talk keep working while the client is in the background, as in 1.x, without a setting (this reading is the assistant's; the owner named the card) |
| Direct messages | No "Message" action on a member. [09](09-feature-parity.md) moves it from "Soon" to "Later" |
| Status text, "Invisible", automatic idle | Presence is Online, Idle or Do not disturb, set by hand; Offline comes from the server |

Kept, although they were offered for removal: the audit log and the
statistics in the party settings, and hardware keys.

### D-31 — Order of work
**Decided by the owner (2026-11-12):** implementation starts on two tracks at
once: the foundations of [10](10-migration-plan.md) Phase 0 (workspace, lints,
CI, `parties-perm`) and the window shell of the Glass design in lurq on
fixture data, which also tests lurq against the design (backdrop blur, frame
time).

## 2. Questions for the project owner

| # | Question | Why it matters |
|---|---|---|
| Q1 | Are there deployments today that must be migrated, or may 2.0 simply replace 1.x? | Decides how much effort the importer and the transition release deserve |
| Q2 | How many people will work on this, and with how much Rust experience? | Turns phase sizes into dates and decides which phases run in parallel |
| Q3 | Answered 2026-11-10: the UI is built with lurq (D-04). Open in its place: who adds zero-copy texture import, the iOS shell and accessibility to lurq, and when? | These sit on the critical path of video, iOS and a public launch |
| Q4 | Are optional project-run services acceptable (push relay, server directory, join-link landing page)? | Without a relay there is no mobile push |
| Q5 | Are Android, a Linux desktop client and a web client on the roadmap? | They change how much weight portability gets in D-04 and in the capture and audio abstractions |
| Q6 | Is end-to-end media encryption required at launch? | D-11; a large scope change |
| Q7 | Should the launch client stay connected to several servers at once? | D-14; affects the main navigation design |
| Q8 | Should the interim hardening of the 1.4 server be done now? | Answered 2026-11-10: yes for the two critical findings (F1, F2), done as protocol 1.5. The rest of [04](04-connection-security.md) section 13 is still open |
| Q9 | What is the final product name and domain? | Deep-link scheme, landing page, bundle identifiers, and the relying-party id of hardware keys and passkeys (D-26, D-28). On Apple platforms passkeys also need a file served from that domain |
| Q10 | Hardware keys: is FIDO2 acceptable as the mechanism, or must the YubiKey's PIV applet be used? Which platforms must support it at launch? | D-26; the two need different code on every platform |
| Q11 | Should a hardware key replace the software key, or be required in addition to it? | D-26; decides the recovery story |
| Q12 | Passkeys: is deriving the key from the passkey acceptable (the key is in the client's memory while unlocked), or must every sign-in be signed inside the passkey, with a system prompt per connection? | Answered 2026-11-11: deriving the key is acceptable (D-28) |
| Q13 | Passkeys: unlock once per device (the key is then also kept in the OS keystore) or at every start of the client? | Answered 2026-11-11: once per device. The passkey is there so the key is not lost, not for security (D-28) |

## 3. How to change a decision

Edit the entry, state what changed and why, and update the documents that
depend on it in the same change. Decisions that a spike settles get the
spike's result linked from their entry.
