# Parties — Project & Network Analysis Backlog

Generated 2026-06-10 from a full architecture + network-protocol review. Each
item has a location, the problem, and a concrete recommendation. Priorities:
**Critical** (do soon), **High**, **Medium**, **Low**.

The transport foundation is solid — MsQuic + QUIC TLS 1.3, bounds-checked
`BinaryReader`, server-stamped sender IDs, length caps on every frame. The
issues are concentrated in application-layer trust decisions, one non-functional
"encryption" feature, threading hazards, and a god-object client.

---

## Security (network)

### Critical
- **S1 — "Channel encryption" is dead crypto.** The server mints a 32-byte
  `CHANNEL_KEY` and the client stores it in `channel_key_`
  (`app_core.cpp on_channel_key`) but it is **never used**. Voice is sent as raw
  Opus (`app_core.cpp:53-61`) and received raw. Media is protected only by QUIC
  TLS hop-by-hop; the SFU sees plaintext. Decide: (a) remove `CHANNEL_KEY` and
  document "transport-encrypted, trusted SFU", or (b) implement per-packet AEAD
  (ChaCha20-Poly1305, nonce = `sender_id‖seq`) — feasible since the SFU forwards
  opaque payloads, but key distribution must move off the server for it to be
  real E2E.
- **S2 — Path traversal in chat file upload.** `server.cpp:1146-1162` builds the
  on-disk path from the client-supplied filename (`reader.read_string()`),
  written at `:217`. `..\..\` or an absolute path escapes the storage dir →
  arbitrary file write. Fix: name files by `attachment_id`/UUID, keep the
  display name only in the DB; if preserving it, reduce to `path.filename()`,
  reject `..`/separators/drive letters, and verify the canonical path stays in
  the storage root.

### High
- **S3 — Auth replayable within ±60s, no nonce/challenge.** `server.cpp:360-366`
  only checks timestamp freshness. Add a per-connection server challenge (server
  sends 32-byte nonce on stream open; client signs `nonce‖pubkey‖name‖ts`), or
  at least a short-lived seen-`(pubkey,ts,sig)` replay cache.
- **S4 — File upload has no ownership check.** `server.cpp:199-229` never
  verifies the uploader authored the message owning the attachment. Join
  attachment→message→author and require `session.user_id == message.author_id`.
- **S5 — 0-RTT enabled atop replayable auth.** `quic_server.cpp:61`
  (`QUIC_SERVER_RESUME_AND_ZERORTT`). 0-RTT early data is replayable; combined
  with S3 this is a latent footgun. Disable 0-RTT unless it gives measurable
  connect-latency wins, or gate which message types are early-data-safe.

### Medium
- **S6 — Upload size limit enforced only after the whole file is buffered in
  RAM.** `quic_server.cpp:727-731` accumulates unbounded; cap checked at
  `server.cpp:211` after `PEER_SEND_SHUTDOWN`. Enforce incrementally during
  `QUIC_STREAM_EVENT_RECEIVE` and abort the stream when exceeded.
- **S7 — No rate limiting anywhere.** No per-session caps on control messages,
  chat sends, channel joins, voice datagrams, stream creation. Add token buckets
  (tighter on DB-touching/broadcasting ops).
- **S8 — No per-source rate limit on the connectionless query.** Anti-
  amplification (reply ≤ request) is correct, but a spoofed-source flood still
  bounces 1:1 traffic. Add a per-source token bucket; add a regression test
  asserting reply size ≤ request size.
- **S9 — Non-constant-time password compare.** `server.cpp:388`
  (`client_password != config_.server_password`). Use a constant-time compare.

### Low
- **S10** unknown message types silently dropped (no debug log).
- **S11** `random_bytes` ignores `RAND_bytes` return value (`crypto.cpp:32`) —
  used for session tokens & channel keys; treat failure as fatal.
- TOFU uses `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` by design; ensure
  the TOFU-mismatch path is a hard stop (it is). RSA-4096 self-signed cert with
  no SAN stalls first boot — consider Ed25519/P-256.

---

## Robustness (protocol)
- **R1 (High) — Version check is exact-match.** `server.cpp:341-346`
  (`client_version != PROTOCOL_VERSION`) breaks all older clients on any minor
  bump despite the header promising backwards-compatible minors. Split
  major/minor; reject only on major mismatch.
- **R4 (High) — `SERVER_ERROR` is a free-form string, no codes.**
  (`protocol.h:38`). The client can't distinguish "wrong password" (retry) from
  "version mismatch" (must update). Add a `u16` code as the first field. *(The
  auto-reconnect work string-matches "kicked"/"another location" as a stopgap —
  error codes would replace that.)*
- **R2 (Medium) — A single malformed length flushes the whole stream buffer**
  (`net_client_parsing.h:31`, `quic_server.cpp:630`) → permanent framing desync
  with no teardown. On a framing violation, `ConnectionShutdown` (fail closed).
- **R3 (Medium) — Keepalive layering is redundant.** QUIC `KeepAliveIntervalMs`
  15s + `IdleTimeoutMs` 60s + app `KEEPALIVE_PING` every **2s**
  (`app_core.cpp:223-232`). The 2s ping exists only for the RTT readout; slow it
  to 5-10s or read `QUIC_PARAM_CONN_STATISTICS.SmoothedRtt` and drop it.
- **R5 (Medium) — Stream role inferred from arrival order**
  (`quic_server.cpp:431-439`): "first peer stream = control, second = video".
  QUIC doesn't guarantee start-notification order. Tag each long-lived stream
  with a 1-byte type (already done for file streams).
- **R6 (Medium) — `disconnect()` sleeps 100ms** (`net_client_msquic.cpp:144`)
  instead of awaiting `SHUTDOWN_COMPLETE`. Gate handle cleanup on the event.

---

## Efficiency
- **E1 (Medium) — Two heap allocations per outbound message** (`new uint8_t[]` +
  `new QUIC_BUFFER`) on every send incl. the SFU fan-out hot path. Pool the
  buffer+data blocks; for fan-out, the bytes are identical across recipients.
- **E3 (High) — Chat bulk traffic shares the control stream** → head-of-line
  blocking. A large `CHAT_HISTORY_RESP`/`CHAT_SEARCH_RESP` blocks presence/
  voice-state messages behind it. Move bulk request-response chat onto a
  separate QUIC stream.
- **E4 (Medium) — Voice fan-out is O(M·N) per packet** (`server.cpp:169-180`
  snapshots all sessions per datagram). Maintain a per-channel membership index.
- **E2 (Low)** — `buffer.erase(begin, begin+n)` after each parsed frame is O(n)
  memmove; track a read offset, compact lazily.

---

## Extensibility
- **X1 (Medium) — No request/response correlation ID.** History/search/admin
  responses can't be matched to their request, can't have two in flight, can't
  time out individually. Add a client-chosen `u32 request_id` echoed in
  responses.
- **X3 (Medium) — No capability negotiation.** Only `PROTOCOL_VERSION`
  (exact match). Add a feature bitmap exchange so additive features don't need a
  breaking version bump (companion to R1).
- **X2 (Low)** — flat hand-numbered `u16` enum with manual ranges; document the
  scheme, consider a direction bit (high bit = server→client) to detect
  misrouting.

---

## Architecture / threading
- **C1 (Critical) — Server mutates `Session` fields from the MsQuic disconnect
  callback without `sessions_mutex_`,** racing the main loop. Enqueue
  disconnects and drain on the main loop (mirror the client's
  `disconnect_pending_` pattern).
- **C2 (Critical) — Client touches `viewing_sharer_`, `awaiting_keyframe_`, and
  the shared `settings_` sqlite handle from the MsQuic worker thread,** bypassing
  the documented marshal-to-tick() rule. Move these mutations onto the main
  thread.
- **H1 (High) — `app_core.cpp` (~2.4k lines) is a god-object.** Extract a
  `ConnectionController` (where auto-reconnect now lives), `ServerMessageRouter`,
  and `ChatController`.
- **H4 (High) — Server send paths drop on backpressure silently** with no
  failure tracking.
- **M (Medium)** — unbounded per-session recv buffers; whole-file-in-RAM
  transfers (see S6); Win32 save-file dialog leaking into shared `AppCore`
  (`app_core.cpp` tick); triplicated QUIC framing/buffer-ownership code; per-
  frame double-copy on the video forward path; 1ms busy-poll server loop.

---

## Testing
- One happy-path integration test only. **Zero unit tests** for serialization,
  stream reassembly, or permissions — the three pure, security-critical,
  highest-ROI targets. Add these first.

---

## Suggested order
1. S2 (path traversal) — small, highest severity.
2. S4 + S6 (upload auth + incremental size cap) — same code area.
3. C1 + C2 (the two data races) — de-risk everything else.
4. S1 (channel key) — decide remove vs. real E2E; it's actively misleading now.
5. R1 + R4 (versioning + error codes) — unblocks graceful protocol evolution.
6. E3 (split chat off the control stream) — latency win.
7. Unit tests for serialization / reassembly / permissions.
