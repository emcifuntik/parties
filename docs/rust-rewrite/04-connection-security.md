# 04 — Connection security: review and design

> Status: Draft — 2026-11-10. Review of the C++ implementation at commit
> `df60746`; line numbers refer to that commit. The two critical findings were
> re-checked by hand against the code.

## 1. Scope and method

The review covered everything between a UDP packet arriving and a request
being executed: QUIC and TLS configuration, server trust, user
authentication, admission of connections, authorization of every control
message, resource limits, secrets at rest on both sides, the connectionless
server query, and the update channel. It read the code, not the docs:
`docs/protocol.md` sections 10 and 11 no longer describe what is implemented.

Each finding is tagged **Impl** (a bug a careful rewrite removes) or
**Design** (the protocol or trust model must change). The right-hand column
points at the section of this document, or the document, that closes it.

## 2. Findings in the current implementation

### Critical

| ID | Finding | Evidence | Tag | Closed by |
|---|---|---|---|---|
| F1 | **The login signature can be relayed to another server.** The client signs only `public key ‖ display name ‖ timestamp`. Nothing ties the signature to the server or the connection. Any server a user connects to can replay that login to a different server within the 30-second window and act as that user there, including as owner. The replay guard is per server and does not help. | `client/src/app_core.cpp:900-903`, `server/src/server.cpp:463-475` | Design | §5.1 |
| F2 | **Unauthenticated memory exhaustion through file streams.** The third and later bidirectional streams become file streams without an authentication check. Their data is appended to a buffer with no limit until the stream ends; the 50 MB cap is checked afterwards. Eight streams per connection, unlimited connections. | `server/src/quic_server.cpp:866-896`, `1218-1222`; `server/src/server.cpp:274` | Impl | §6, §8 |

### High

| ID | Finding | Evidence | Tag | Closed by |
|---|---|---|---|---|
| F3 | No limits before authentication: `max_clients` and `listen_ip` are read from config but never applied, there is no auth deadline, and no per-address cap | `quic_server.cpp:253-365`, `781-811` | Impl + Design | §6 |
| F4 | Unbounded application queues and no rate limit on control messages (only keyframe requests are limited). Frames up to 1 MiB are queued even before authentication | `quic_server.cpp:1110-1136`, `908-919`; `server.cpp:182-187` | Impl | §8 |
| F5 | The server password can be guessed without limit (a failed attempt neither disconnects nor delays) and is sent as plaintext inside TLS, so a man in the middle on a first connection learns it | `server.cpp:477-489`; `app_core.cpp:922` | Design | §5.3, §6 |
| F6 | There are no bans. A kicked user reconnects immediately, and a ban on a key would be useless because a new key is free | `server.cpp:1054-1081` | Design | §6.3 |
| F7 | Most permission bits are never checked: `Speak`, `SendText`, `UploadFiles`, `ShareScreen`, `ShareWebcam`, `MuteOthers`, `DeafenOthers`, `ManagePermissions`, `ManageServer`. The defaults claim ordinary users cannot send text or share; the server lets them | `common/include/parties/permissions.h:15-32` against `server.cpp` | Impl | [05](05-permissions-and-roles.md) §7 |

### Medium

| ID | Finding | Evidence | Tag | Closed by |
|---|---|---|---|---|
| F8 | Any user can pin or unpin any message | `server.cpp:1484-1548` | Impl | [05](05-permissions-and-roles.md) |
| F9 | Text channels and attachments have no access control: history, search and downloads accept any id; attachments of deleted messages stay downloadable; chat is broadcast to every session | `server.cpp:1432-1481`, `1585-1647`, `306-334` | Design | [05](05-permissions-and-roles.md), [03](03-protocol-v2.md) §7 |
| F10 | Trust on first use is silent, a changed key is accepted with one click, pins are keyed by host name, and an empty fingerprint is treated as "resumed, trusted" | `app_core.cpp:873-888`, `3225-3234`; `client/src/settings.cpp:203-223` | Design | §4 |
| F11 | The seed phrase, the secret key and saved server passwords are stored in plain SQLite on every platform | `settings.cpp:53-77`, `144-160` | Impl | §9 |
| F12 | Key derivation is `SHA-256(phrase as typed)`: no checksum, no normalisation (a trailing space yields a different identity), and import accepts any twelve dictionary words | `common/src/crypto.cpp:122-170` | Design | §5.2 |
| F13 | Display and channel names are not validated (empty, 64 KiB, any bytes) and are logged raw | `server.cpp:420`, `521-525`, `865`, `958` | Impl | §7 |
| F14 | Each download reads the whole file into memory and copies it again; no limit on concurrent downloads | `server.cpp:325-332` | Impl | [03](03-protocol-v2.md) §7 |
| F15 | A framing error clears the buffer and continues instead of closing the connection | `quic_server.cpp:1117-1121`; `client/src/net_client_parsing.h:32-35` | Impl | [03](03-protocol-v2.md) §3 |
| F16 | Stream roles depend on the order in which streams are opened | `quic_server.cpp:866-886` | Design | [03](03-protocol-v2.md) §2 |
| F17 | The Windows updater accepts a binary signed by **any** trusted publisher, does not check revocation, does not restrict the download URL, and verifies in a temp directory before a later copy | `client/src/windows/auto_updater.cpp:222-244`, `393-416` | Design | §11 |
| F18 | The connectionless query has no per-source rate limit, its reply is not authenticated, and it reveals user count and "password required" to anyone | `vendor/ports/msquic/0001-unconnected-query.patch`; `quic_server.cpp:740-779` | Design | [03](03-protocol-v2.md) §8 |

### Low and informational

| ID | Finding | Tag | Closed by |
|---|---|---|---|
| F19 | RSA-4096 certificate with serial 1 and no SAN; key file written with default permissions; the container runs as root | Impl | §4, §9 |
| F20 | `server.default.toml` contains keys the server does not read, including `admin_password = "changeme"` | Impl | [07](07-server-management.md) |
| F21 | A failed random-number call is not fatal; callers ignore the result | Impl | By construction: `getrandom` failure aborts |
| F22 | Role values are not range-checked | Impl | Typed enums |
| F23 | Kick reports success after a hierarchy check failed | Impl | Single authorization gate |
| F24 | Keep-alive link reports are served before authentication | Impl | Pre-auth state machine |
| F25 | The session token is generated, sent and never used | Design | Removed |
| F26 | Resumption tickets are issued to unauthenticated peers and stored in plaintext | Design | [03](03-protocol-v2.md) §1 |
| F27 | One public key identifies the user on every server, so operators can correlate users | Design | §5.4 |
| F28 | Crash reports attach the log and memory that may contain the secret key | Impl | §12 |

### State of the June 2026 backlog

Verified fixed in code: S1, S2, S3 (replay cache only), S4, S5, S9, S10, R1,
R4, C1. Partly fixed: S11 (failure is reported but ignored, F21). Still open:
S6 (now F2, and worse than recorded because it works before authentication),
S7 (F4), S8 (F18), R2 (F15), R5 (F16).

### Worth keeping

- The server stamps the sender on everything it forwards; a client cannot
  speak as someone else.
- Bounds-checked parsing, a length cap on every frame, prepared SQL
  statements everywhere.
- 0-RTT is off; the major/minor version split and typed error codes exist.
- Attachment files have server-generated names and an ownership check on
  upload.
- Video ingress is accepted only from active publishers and is byte-capped;
  keyframe requests are rate limited and coalesced.
- The query reply is never larger than the request.
- Identity is a key pair; the server stores no user passwords.

## 3. Threat model for 2.0

| Adversary | Capability | What they must not achieve |
|---|---|---|
| Network observer | Reads all packets | Learn content, identities or channel activity |
| Active network attacker | Intercepts, modifies, replays, spoofs source addresses | Impersonate the server or a user; downgrade; learn join credentials |
| Unauthenticated client | Any number of connections from many addresses | Exhaust memory, CPU, disk or file handles; reach any state-changing code |
| Authenticated member | Valid identity, minimal permissions | Act outside their permissions; see hidden channels; impersonate others; degrade service for others |
| Another server's operator | Runs a server the user also connects to | Use the user's login anywhere else; learn the user's secrets |
| Malicious server | The user connects to it deliberately | Compromise the client device; read other servers' data or the user's key |
| Device thief, local malware without admin rights | Reads the user's files | Extract the identity key from application data alone |
| Update-channel attacker | Controls a mirror, the network path, or obtains some code-signing certificate | Get a binary installed |

Trust assumptions, stated plainly: the server operator can read media and
messages in 2.0 (hop-by-hop encryption, §10). The user's operating system
account is trusted. The server's host is trusted by its operator.

## 4. Server identity and trust

Each server has a long-term **identity key** (Ed25519), created on first
start and stored with owner-only file permissions. The server's identity is
the hash of that public key, shown to people as six words.

| Mode | TLS certificate | How the client decides to trust |
|---|---|---|
| Self-signed (default) | Self-signed with the identity key, generated automatically | The key must match a pin |
| CA-issued | Operator-supplied chain for a host name, reloadable without restart | Standard WebPKI validation of the host name; the identity key is still proven in `HelloAck` and pinned on first contact |

How a pin is obtained, in order of preference:

1. **From a join link or QR code.** Links have the form
   `parties://host:port/j/<join code>#k=<server key>`. The key travels in the
   fragment. This is the normal way to join and gives authenticated first
   contact.
2. **From a previous connection** (the stored pin).
3. **Confirmed by the user.** When neither exists, the client finishes the
   handshake, sends nothing, and shows the server's six words with the choice
   to trust or cancel. Nothing is trusted silently.

A pinned server that presents a different key is a hard failure. The connect
path offers no "accept anyway" button. Re-pinning is a deliberate action in
the server's entry in the client, or happens automatically when the server
presents a valid **rotation statement**: the new key signed by the old key.

Pins are stored per server record, keyed by the identity key, not by host
name; a server that moves to a new address keeps its identity.

## 5. User identity and authentication

### 5.1 Authentication is bound to the connection

Specified in [03](03-protocol-v2.md) section 4. The user's signature covers a
TLS exporter value and the server's identity key. A signature made for one
connection verifies on no other connection and no other server, which removes
F1 and makes timestamps and replay caches unnecessary. The server proves
possession of its identity key the same way.

### 5.2 Key derivation and continuity

| Version | Derivation | Use |
|---|---|---|
| v1 | `SHA-256(phrase bytes)` → Ed25519 seed | Import of existing identities only. The same phrase keeps the same public key, so memberships and roles survive the migration |
| v2 | Standard BIP-39 mnemonic with checksum → PBKDF2-HMAC-SHA512 seed → HKDF-SHA256 with a domain label → Ed25519 seed | All new identities |

The client records which version an identity uses. Import tries both and
shows the resulting fingerprints when it cannot tell. New phrases are always
generated, never chosen by the user.

### 5.3 Join credentials

A password is no longer needed on every connection. It is one of several ways
to **become a member**; after that the identity key is enough.

| Policy | A new identity becomes a member when |
|---|---|
| Open | It connects |
| Password | It presents a valid `password_proof` |
| Join code | It presents a valid, unused join code |
| Approval | A moderator approves the pending request |

The password never crosses the wire. The proof is an HMAC over the exporter
keyed by an Argon2id hash of the password, so an attacker in the middle of a
first connection cannot relay it and can only attack it offline at Argon2id
cost. The server stores the Argon2id output, not the password.

Join codes are single-use by default and are specified in
[13](13-profiles-access-and-hardware-keys.md) section 5.

### 5.4 Reserved extensions

- **Device keys.** `Authenticate` carries an optional certificate chain so a
  device key, certified by the root identity, can log in instead of the root
  key. This lets the recovery phrase stay off the device and lets a lost
  device be revoked. Not implemented in 2.0; the field exists so it needs no
  protocol break.
- **Hardware-backed keys.** A user may sign in with a key held on a YubiKey
  instead of a software key, and an account may hold several keys. The
  channel binding is the same. See
  [13](13-profiles-access-and-hardware-keys.md) section 6.
- **Passkey-held keys.** A user may keep the identity key in a passkey: the
  key is derived from a secret the passkey releases after verifying the user.
  The server sees an ordinary key, so nothing in this document changes for
  it. It is a copy against losing the key, not a protection: whoever controls
  the passkey provider's account can obtain the key. See
  [13](13-profiles-access-and-hardware-keys.md) section 7.
- **Per-server identities.** Deriving a separate key per server from the same
  phrase stops operators correlating users (F27). It is an opt-in for a later
  release because it also removes the ability to prove the same identity on
  two servers.

## 6. Admission control and abuse resistance

### 6.1 Before authentication

| Control | Rule |
|---|---|
| Banned source address | Packet ignored, no reply |
| Address validation | QUIC Retry when the source is not validated and the server is under load, or always if the operator enables it |
| Connection caps | Global cap on unauthenticated connections; per-address caps on unauthenticated and total connections; IPv6 counted per /64 |
| Deadline | 10 s to authenticate, independent of keep-alives |
| Budget | One stream, 4 KiB frames, no datagrams, no file or media streams |
| Failed authentication | The connection is closed; the address gets an exponential delay before its next attempt |
| Join credential failures | Counted per address and globally; lockout and an audit entry on a burst |

### 6.2 After authentication

Rate classes per request type ([03](03-protocol-v2.md) section 9), bounded
queues everywhere, slow consumers disconnected, per-member limits on streams
and uploads from role limits ([05](05-permissions-and-roles.md) section 6).

### 6.3 Bans and free identities

A ban names an identity, an address range, or both, with an optional expiry
and a reason. Because a new identity costs nothing, a ban on a key alone only
stops honest users. Four mechanisms give bans teeth; operators combine them:

| Mechanism | Effect |
|---|---|
| Join policy other than open | A new identity needs a join code, the password or approval |
| Address bans and per-address registration limits | Limits how many new identities one address can enrol per day |
| Identity level | A proof of work attached to the public key (leading zero bits of a hash over key and counter). The server sets a minimum. Computed once, reusable on every server, the same idea as TeamSpeak's security level |
| New-member restrictions | Members younger than N minutes, or without a role, cannot speak, send or stream |

## 7. Authorization and input validation

- Every request passes one gate that looks up the required permission and
  hierarchy rule in a table. The match over request types is exhaustive, so a
  new request cannot be added without stating its rule. See
  [05](05-permissions-and-roles.md) section 7.
- Visibility is part of authorization: lists, events, history, search,
  downloads and member lists are filtered by `ViewChannel`.
- Media is authorized continuously, not only at start. Losing a permission
  stops the track.
- Text input is validated once, at the boundary, into typed values:

| Input | Rule |
|---|---|
| Display names, nicknames | NFC, trimmed, 1–32 characters, no control, bidirectional-override or invisible formatting characters; optional uniqueness per server |
| Channel and role names | Same character rules, 1–64 characters |
| Messages | Length limit in characters, no NUL |
| File names | Metadata only; path separators and control characters removed; never used to build a path |
| Media types | Determined by the server from content, not taken from the client |
| Identifiers and enums | Typed; unknown enum values are rejected, not stored |

The client applies the mirror-image rules to data from the server: every
string from the network is untrusted text, images are decoded with size and
memory limits, downloads are never opened automatically, and a
`parties://` link always leads to a confirmation dialog.

## 8. Resource limits

Named bounds for every buffer and queue are listed in
[03](03-protocol-v2.md) section 9. The rules behind them:

1. Nothing is buffered without a cap that is checked while receiving.
2. Uploads and downloads stream to and from disk.
3. Per-connection memory before authentication is a small constant.
4. Database work for one request is bounded (page sizes, search limits,
   statement timeouts) and never runs on the task that forwards media.
5. Disk use is bounded by the retention and quota settings; hitting a quota
   refuses uploads instead of failing writes.

## 9. Secrets at rest

| Secret | Where it lives in 2.0 |
|---|---|
| User identity key and recovery entropy | OS keystore: DPAPI-backed credential on Windows, Keychain on macOS and iOS. Never in the SQLite file. Viewing the recovery phrase asks for OS-level user presence where available |
| User identity key kept in a passkey | In the passkey provider, and in the OS keystore of each device after its first unlock there, like a software key. The passkey is a copy against loss, not extra protection. See [13](13-profiles-access-and-hardware-keys.md) section 7.3 |
| Server pins, preferences | Client database (not secret, integrity matters) |
| Saved join passwords | Not stored; membership replaces them |
| Server identity key, TLS key | Data directory, owner-only permissions, refused if group- or world-readable on Unix |
| Join password | Argon2id hash in the server database |
| Join codes, ownership claim token | Stored hashed; shown once |

Secret types are zeroed on drop and cannot be formatted for logs. Copying the
recovery phrase marks the clipboard entry as sensitive where the OS supports
it and clears it after a minute. The server container runs as a non-root
user.

## 10. Media confidentiality

In 2.0, media and messages are encrypted between each client and the server.
The operator of a server can access them. This is the same trust model as
today, and it must be described honestly: the README's "end-to-end encryption
via QUIC" claim is wrong and has to change before a public launch.

End-to-end media encryption is a planned minor-version feature (decision
D-11). The protocol reserves what it needs.

## 11. Updates

The update channel is part of connection security: it is how an attacker
reaches every user at once.

- Update metadata and packages are signed with a dedicated release key
  (Ed25519). The public key is compiled into the client. The signature is
  checked on the exact bytes that get installed.
- Platform code signing (Authenticode, Developer ID, notarisation) stays, as
  a second, independent check. On Windows the client also pins the expected
  publisher.
- Downloads are HTTPS from an allow-listed host; version downgrades are
  refused.
- The server binary and container image are signed and ship with checksums
  and a software bill of materials.

Details and tooling: [11-quality-and-release.md](11-quality-and-release.md).

## 12. Logging and privacy

- Logs never contain keys, tokens, join codes, message text or recovery
  phrases. Peer-supplied strings are escaped and truncated.
- Crash reporting is opt-in on the client, scrubs known secret memory by
  keeping secrets in locked, non-dumped allocations where the OS allows, and
  attaches no log lines above a size cap.
- The server records IP addresses only where a security control needs them
  (bans, rate limiting, audit) and expires them on a schedule the operator
  sets.

## 13. Interim hardening of 1.4.x

The rewrite will take months and the current server is already deployed. These
fixes are small and worth making in the C++ code now, in this order:

| # | Fix | Finding | Status |
|---|---|---|---|
| 1 | Refuse file streams from unauthenticated sessions; enforce the size cap while receiving | F2 | Done in protocol 1.5 |
| 2 | Add an authentication deadline; apply `max_clients`; add a per-address connection cap | F3 | Open |
| 3 | Close the connection on a failed login and delay the address | F5 | Open |
| 4 | Add a server challenge: the client signs a server nonce and the certificate fingerprint it observed | F1 | Done in protocol 1.5 |
| 5 | Check the permission bits that exist, or correct the defaults; require a permission for pin/unpin; refuse downloads of deleted attachments | F7, F8, F9 | Open |
| 6 | Pin the expected publisher in the Windows updater and verify the installed bytes | F17 | Open |
| 7 | Correct the README's encryption wording; delete the stale default config; run the container as non-root | §10, F20, F19 | Open |

What protocol 1.5 does (2026-11-10), as described in `docs/protocol.md`:

- **F1.** The server sends `AUTH_CHALLENGE` when the control stream opens. A
  1.5 client signs the nonce and the fingerprint of the certificate it saw;
  the server verifies with its own. This is not additive: a 1.5 server refuses
  the old signature unless the operator sets `allow_legacy_auth`, and a 1.5
  client refuses a server that sends no challenge, because falling back would
  let a hostile server withhold the challenge. Servers and clients therefore
  have to be updated together.
- **F2.** A file-transfer stream is admitted before any of its data is kept:
  the session must be authenticated, the attachment must have been announced
  by that session, and the stream may carry exactly the announced size.
  Memory is bounded by reserving the announced size per session and
  server-wide; an upload that does not fit waits under QUIC flow control
  instead of being refused, so honest multi-file uploads are not lost. The
  server now also closes a completed upload stream; before, each finished
  upload kept one of the connection's eight file-stream slots and the ninth
  transfer on a connection never started (reproduced, then fixed).
- **Resumption tickets (part of F10).** The client used to store a ticket from
  every server it connected to, including one whose changed certificate the
  user then rejected. A resumed session exchanges no certificate, so the next
  connection to that address would have skipped the pin check, and the bound
  login (which then uses the pinned fingerprint) could have been relayed to
  the real server after all. The client now stores a ticket only once the
  server's certificate has matched the pin, discards tickets on rejection, and
  drops all tickets stored under the old rule once.
- Regression tests: `tests/connection_security_test.cpp` covers the server
  side of both fixes. The client-side rules (wait for the challenge, refuse a
  server that sends none, the resumption rule) live in the client application
  and have no automated test.

Known limits of the 1.5 fixes, left for the rewrite or a later patch:

- A session that is admitted for an upload and then stops sending keeps its
  reservation until it disconnects. One session can hold a quarter of the
  server-wide reservation this way; four identities can stall everyone's
  uploads (not exhaust memory). A stall timeout would close this.
- The main loop may still use a download stream's handle after the peer
  aborted the stream (seen in code, not introduced or fixed here).
- `parties_voice_congestion` is a flaky test: on the unmodified code it failed
  3 of 20 runs, always because the control stream starved behind voice
  datagrams for the rest of the congested phase. With 1.5 it failed 5 of 14
  runs and answered fewer pings per run (6.4 against 7.9). Nothing on that
  path changed after login, and reverting either connection-setup change by
  itself gave in-between results, so the cause is not established. The likely
  explanation, not verified, is that the congestion controller's state when
  the test starts shaping depends on the few packets exchanged at login. Worth
  re-measuring on a connection that has carried voice for a while first.

## 14. Verification plan

| Activity | When |
|---|---|
| Fuzz targets for every parser: control frames, media headers, query packets, join links, stream prefixes | From Phase 0, run continuously |
| Authorization matrix test: every request type against a member without the permission, and against hidden channels | Phase 1 onward, generated from the requirement table |
| Handshake tests: relay attempt, wrong exporter, wrong server key, replay, pin mismatch, rotation statement | Phase 1 |
| Pre-authentication abuse tests with the test kit: connection floods, slow handshakes, oversize frames, stream floods | Phase 1 |
| Dependency audit and licence checks in CI | Phase 0 onward |
| Independent security review of protocol and server | Before public launch |
| Published disclosure policy (`SECURITY.md`) | Before public launch |
