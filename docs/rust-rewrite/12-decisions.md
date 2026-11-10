# 12 — Decision log and open questions

> Status: Draft — 2026-11-10. Every decision below is **proposed**. None has
> been confirmed by the project owner yet. Decisions marked "gated" are
> settled by a Phase 0 spike.

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

### D-04 — UI toolkit (gated: UI spike)
**Proposed:** Slint. **Fallback:** egui. **Contingency:** keep the RmlUi
shell on top of the Rust core through a C interface for a transition period.
**Alternatives considered:** a web view (Tauri), native UI per platform.
**Why and criteria:** [08](08-client-design.md) section 6.1.
**Needs from the owner:** confirmation that the licence is acceptable
(question Q3).

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
**Proposed:** a long-term server identity key, pinned by clients. Invite
links carry the key. CA-issued certificates are optional. A new key is never
trusted silently, and a changed key is a hard failure unless a rotation
statement is presented. See [04](04-connection-security.md) section 4.

### D-09 — Joining a server
**Proposed:** join policies (open, password, invite-only, approval) decide
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

### D-12 — Temporary codec bridge
**Proposed:** the existing `encdec` library may be linked behind the Rust
codec traits under a feature flag until each vendor backend is ported. It is
the only C++ that may ship in a Rust build, and it is removed before the C++
tree is.

### D-13 — Repository layout
**Proposed:** the Cargo workspace lives at the root of this repository next
to the CMake tree until cutover.
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
licensed unless an exception is recorded here. Known items to clear: the UI
toolkit (D-04), the WebRTC audio library, and H.265, which is used only
through operating-system and hardware encoders and decoders.

### D-19 — Codec negotiation
**Proposed:** participants advertise what they can decode; the publisher
picks a codec everyone in the room supports, with H.264 as the floor. The
bundled software H.265 decoder is dropped.

### D-20 — No 0-RTT, no resumption in 2.0
**Proposed:** both off. Authentication never depends on TLS session state.

## 2. Questions for the project owner

| # | Question | Why it matters |
|---|---|---|
| Q1 | Are there deployments today that must be migrated, or may 2.0 simply replace 1.x? | Decides how much effort the importer and the transition release deserve |
| Q2 | How many people will work on this, and with how much Rust experience? | Turns phase sizes into dates and decides which phases run in parallel |
| Q3 | Is the Slint licence acceptable, or are permissive-only dependencies a hard rule? Is a non-Rust UI layer out of the question? | Decides D-04 before the spike starts |
| Q4 | Are optional project-run services acceptable (push relay, server directory, invite landing page)? | Without a relay there is no mobile push |
| Q5 | Are Android, a Linux desktop client and a web client on the roadmap? | They change how much weight portability gets in D-04 and in the capture and audio abstractions |
| Q6 | Is end-to-end media encryption required at launch? | D-11; a large scope change |
| Q7 | Should the launch client stay connected to several servers at once? | D-14; affects the main navigation design |
| Q8 | Should the interim hardening of the 1.4 server be done now? | Answered 2026-11-10: yes for the two critical findings (F1, F2), done as protocol 1.5. The rest of [04](04-connection-security.md) section 13 is still open |
| Q9 | What is the final product name and domain? | Deep-link scheme, landing page, bundle identifiers |

## 3. How to change a decision

Edit the entry, state what changed and why, and update the documents that
depend on it in the same change. Decisions that a spike settles get the
spike's result linked from their entry.
