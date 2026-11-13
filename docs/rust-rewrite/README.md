# Parties in Rust — design documents

> Status: first draft, 2026-11-10. Decisions are proposals unless
> [12-decisions.md](12-decisions.md) marks them as decided by the project
> owner. Implementation started on 2026-11-12; as of 2026-11-13 the Cargo
> workspace holds four crates of Phase 0 and of the first window:
> `parties-perm` (the permission engine), `parties-identity` (keys, recovery
> phrases, handshake proofs, server trust), `parties-proto` (the first wire
> schema, framing, media headers, limits) and `parties-ui` (the Glass window
> on fixture data: a party, Home and Settings). Nothing connects to anything
> yet: there is no transport, no server and no client core.

This folder plans the complete rewrite of the Parties server and client in
Rust: what exists, what the target is, how the protocol and permission system
change, what a security review of the current connection handling found, and
how to get from one to the other.

## Requirements this plan answers

- Server and client in Rust.
- Client on Windows, macOS and iOS; server on Windows and Linux.
- Still QUIC.
- Roles and permissions as flexible as Discord's.
- All server management available from the client.
- A connection-security review, with its findings fixed by design.
- Feature suggestions for parity with Discord and TeamSpeak ahead of a public
  launch.

## Documents

| # | Document | Read it for |
|---|---|---|
| 01 | [Current state](01-current-state.md) | What the C++ system does per platform, what must survive, what is left behind |
| 02 | [Target architecture](02-target-architecture.md) | Workspace, crates, runtime model, technology mapping |
| 03 | [Protocol 2.0](03-protocol-v2.md) | Handshake, control plane, media plane, limits |
| 04 | [Connection security](04-connection-security.md) | The review's 28 findings and the security design that closes them |
| 05 | [Permissions and roles](05-permissions-and-roles.md) | Permission catalogue, resolution, hierarchy, enforcement |
| 06 | [Server design](06-server-design.md) | Admission, world task, media forwarding, storage, deployment |
| 07 | [Server management](07-server-management.md) | Ownership claim, admin surface in the client, `partiesctl` |
| 08 | [Client design](08-client-design.md) | Core, audio, video, UI toolkit choice, platform shells |
| 09 | [Feature parity](09-feature-parity.md) | Discord and TeamSpeak comparison with a recommended launch scope |
| 10 | [Migration plan](10-migration-plan.md) | Phases, exit criteria, porting map, risks, cutover |
| 11 | [Quality and release](11-quality-and-release.md) | Tests, supply chain, signing, updates |
| 12 | [Decisions](12-decisions.md) | Decision log and the questions that need the owner's answer |
| 13 | [Members, profiles, join codes, hardware keys, passkeys](13-profiles-access-and-hardware-keys.md) | Presence, profile pictures, renaming, join codes, YubiKey sign-in and keys kept in a passkey |

Suggested order: 12 for the open questions, then 01, 02, 10 for the shape of
the work, then the topic documents as needed.

## The plan in brief

| Topic | Proposal |
|---|---|
| Strategy | Build in parallel, freeze C++, cut over once; protocol 2.0 breaks compatibility with 1.4 |
| Transport | `quinn` + `rustls`, confirmed by a spike that must pass the existing network-impairment tests |
| Protocol | Typed streams, Protocol Buffers for control, fixed headers for media, authentication bound to the TLS session |
| Trust | Server identity keys pinned through join links; no silent trust on first use |
| Permissions | Discord's model plus numeric role limits; one authorization gate that every request must pass |
| Management | Same connection, same permission gate; client UI and a CLI over the same requests |
| Client | Headless core, thin UI built with lurq (decided); zero-copy video display, an iOS shell and accessibility are work items in lurq |
| Codecs | Rust wrappers for AMF and NVIDIA's codec SDK, and a full port of `encdec` on top of them (decided); no C++ bridge |
| Performance | A requirement (decided): zero-copy media paths, no leaks, memory ceilings, all checked in CI |
| Access | Single-use join codes admit a new identity; the server remembers its fingerprint (decided) |
| Profiles | Members shown online and offline, pictures synchronised by SHA-512, rename yourself in place (decided) |
| Hardware keys | Optional sign-in with a YubiKey (decided); FIDO2 proposed as the mechanism |
| Passkeys | The identity key can be kept in a passkey (decided); proposed mechanism: the key is derived from the passkey, so servers and the protocol do not change |
| Launch scope | The list in [09](09-feature-parity.md) section 11 |

## Findings that should not wait for the rewrite

The security review found two critical issues in the server that is deployed
today:

1. A login signature is not tied to the server it was made for, so any server
   a user connects to can log in as that user elsewhere (F1).
2. An unauthenticated client can exhaust the server's memory through file
   streams (F2).

Both are fixed in the C++ code as protocol 1.5 (2026-11-10); servers and
clients have to be updated together.
[04-connection-security.md](04-connection-security.md) section 13 describes the
fixes and lists the remaining small ones. Separately, the project README
describes transport encryption as end-to-end encryption; that wording should
be corrected now.

## Keeping these documents honest

- They describe intent. When implementation deviates, the document changes in
  the same commit.
- Facts about third-party libraries were checked against their documentation
  on the date above and are re-checked when the relevant phase starts.
- Line references to the C++ code are valid for commit `df60746`.
