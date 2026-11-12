# 09 — Feature parity with Discord and TeamSpeak

> Status: Draft — 2026-11-10. Suggestions for discussion; the tiers are a
> recommendation, not a commitment.

## 1. How to read this

Each feature gets a tier:

| Tier | Meaning |
|---|---|
| **Launch** | Strangers will not adopt or cannot safely run a public server without it |
| **Soon** | Expected within the first releases after launch; the design must not block it |
| **Later** | Worth doing, no urgency |
| **No** | Deliberately not copied |

"Today" is the C++ application at protocol 1.4. D = Discord, TS = TeamSpeak
(3.x, with notes where newer TeamSpeak clients differ).

## 2. Community structure and access

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Roles with hierarchy and per-channel overrides | Yes | Yes (server and channel groups) | Four fixed roles | Launch | [05](05-permissions-and-roles.md) |
| Categories / channel tree | Categories | Nested channels | Flat lists | Launch | One level of categories; deeper nesting is Later |
| Join codes: single-use codes that admit a new identity | Invites | Privilege keys, temporary passwords | No | Launch | A join link also carries the server key, which fixes first-contact trust. [13](13-profiles-access-and-hardware-keys.md) §5 |
| Join codes that grant roles | Via onboarding | Privilege keys | No | Launch | Cheap once join codes exist |
| Join policy: open, password, join code, approval | Screening, verification levels | Passwords, security level | Password only | Launch | [04](04-connection-security.md) §5.3 |
| First-run wizard and server templates | Templates | — | No | Launch (wizard), Later (templates) | |
| Temporary channels, "join to create" | Via bots | Yes | No | Soon | Popular TeamSpeak habit; small feature |
| Channel passwords | No | Yes | No | No | Private channels through overwrites cover it |
| Public server directory | Discovery | Server list | Query protocol only | Soon | Opt-in; see section 9 |
| Welcome screen, rules acceptance | Yes | — | No | Soon | |

## 3. Voice

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Opus voice, FEC, concealment | Yes | Yes | Yes | Launch | Keep |
| Noise suppression | Krisp | Basic | RNNoise | Launch | Keep; a stronger model is Soon |
| Echo cancellation | Yes | Yes | speexdsp | Launch | Upgrade, see D-16 |
| Automatic gain control | Yes | Yes | Normalisation in mixer | Launch | |
| Push-to-talk on every desktop | Yes | Yes | Windows only | Launch | macOS needs the event-tap backend |
| Voice activity detection that works in noise | Yes | Yes | RMS threshold | Launch | Use speech probability |
| Per-user volume and local mute | Yes | Yes | Volume yes | Launch | |
| Microphone test, input level meter | Yes | Yes | Level meter | Launch | Add loopback test |
| Server mute, deafen, move, disconnect | Yes | Yes | Kick only | Launch | |
| Channel user limit and bitrate | Yes | Yes (codec quality) | Limit yes | Launch | |
| AFK channel with idle auto-move | Yes | Via idle time | No | Launch | |
| Speaking indicators without decoding | Yes | Yes | Client-side | Launch | From the level byte |
| Large rooms (loudest-N forwarding) | Yes | — | No | Launch | Needed before a public server fills a room |
| Priority speaker | Yes | Channel commander | No | Soon | Permission bit is reserved |
| Moderated channels, request to speak | Stage channels | Talk power | No | Soon | [05](05-permissions-and-roles.md) §8 |
| Soundboard | Yes | Via plugins | Auxiliary stream exists | Soon | Add clip management UI |
| Whisper to selected users or channels | No | Yes | No | Later | Needs a routing rule and a hotkey UI |
| Voice channel status line | Yes | Channel description | No | Soon | |
| Recording with a visible indicator | No | Yes | No | Later | Consent and legal questions |
| Positional audio | No | Yes | No | No | |

## 4. Video and streaming

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Screen and window share with audio | Yes | TS6 only | Yes (Windows, macOS) | Launch | Keep the latency work |
| AV1 / H.265 hardware encode | Partly, paid tiers | — | Yes | Launch | A real advantage; keep |
| Several simultaneous streams in a grid | Yes | — | Windows only | Launch | All platforms |
| Camera | Yes | TS6 | Windows only | Launch | macOS at launch, iOS with the iOS release |
| Quality limits by role | Paid tiers | Numeric permissions | No | Launch | Role limits |
| Viewer list for the sharer | Yes | — | Count and sound | Launch | |
| Pop-out stream window, fullscreen | Yes | — | Fullscreen | Soon | |
| Simulcast or scalable layers for mixed viewers | Yes | — | No | Soon | Header fields are reserved |
| Screen share from iOS | Yes | — | No | Soon | ReplayKit extension |
| Background blur, virtual background | Yes | — | No | Later | |
| Game overlay | Yes | Yes | No | Later | Windows only, high effort |

## 5. Text chat

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Channels with history and search | Yes | Limited | Yes | Launch | Keep FTS |
| Edit, reply, reactions | Yes | — | Delete and pin only | Launch | Table stakes |
| Mentions, unread and mention badges | Yes | — | No | Launch | Needs read state per member |
| Markdown subset, emoji picker | Yes | BBCode | Plain text | Launch | |
| Attachments on all platforms, paste and drag-and-drop, inline image preview | Yes | File browser | Windows only | Launch | |
| Typing indicator | Yes | Yes | No | Launch | |
| Desktop notifications with per-channel settings | Yes | Yes | Sounds | Launch | |
| Text chat inside voice channels | Yes | Channel chat | Separate text channels | Launch | A voice channel is also a text channel |
| Slow mode | Yes | Anti-flood | No | Launch | Also an abuse control |
| Link previews | Yes | — | No | Soon | Privacy and SSRF trade-off, section 9 |
| Direct messages between members of a server | Global DMs | Private messages | No | Later | Left out of the first version (D-30). Within one server; global DMs are further out still |
| Custom emoji | Yes | Icons | No | Soon | |
| Threads | Yes | — | No | Later | |
| Polls, stickers, voice messages, forum channels | Yes | — | No | Later | |

## 6. Moderation and safety

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Kick, ban (identity and address), timeout | Yes | Yes | Kick only, no ban | Launch | [04](04-connection-security.md) §6.3 |
| Audit log | Yes | Server log | No | Launch | |
| Protection against throwaway identities | Phone/email verification | Security level | None | Launch | Identity level, join policies, new-member restrictions |
| Rate limits and anti-spam | AutoMod | Anti-flood | Keyframe requests only | Launch | Rate classes |
| Member reports to moderators | Yes | Complaints | No | Soon | |
| Word filters, auto-moderation rules | Yes | — | No | Later | |
| Member data export and account deletion | Yes | — | No | Launch | Expected of a public product |
| Honest statement of what the operator can see | — | — | README overstates it | Launch | [04](04-connection-security.md) §10 |
| End-to-end encrypted voice and video | Yes (DAVE) | — | No | Soon | D-11 |

## 7. Identity and profile

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| No account, key-based identity | No | Yes | Yes | Launch | Core of the product; keep |
| Recovery phrase, import and export | — | Identity export | Yes | Launch | Versioned derivation |
| Secure key storage | — | — | Plaintext | Launch | OS keystore |
| Avatar, nickname per server, status | Yes | Avatar, nickname, away | Display name | Launch | Pictures synchronised by hash, rename yourself in place: [13](13-profiles-access-and-hardware-keys.md) §3–4 |
| Presence (online, idle, in voice) and a full member list | Yes | Yes | In voice only | Launch | [13](13-profiles-access-and-hardware-keys.md) §2. Three states set by hand; status text, "Invisible" and automatic idle are left out (D-30) |
| Several devices at once | Yes | One per identity per server | New login replaces old | Soon | Session model supports it from 2.0 |
| Device keys with revocation | — | — | No | Soon | Wire field reserved |
| Sign in with a hardware security key | Yes (as a second factor) | — | No | Launch, optional | [13](13-profiles-access-and-hardware-keys.md) §6 |
| Identity key kept in a passkey | Yes (passkey login to an account) | — | No | Launch, optional | Decided (D-28); depends on the Phase 0 PRF spike. [13](13-profiles-access-and-hardware-keys.md) §7 |
| Friends, activity status | Yes | Contacts | No | Later / No | Activity tracking is not planned |

## 8. Platforms, administration, integrations

| Feature | D | TS | Today | Tier | Notes |
|---|---|---|---|---|---|
| Windows, macOS clients | Yes | Yes | Yes | Launch | |
| iOS client with background voice | Yes | Yes | Receive-only video, no CallKit | Launch | Phase 6 |
| Windows installer, auto-start, signed auto-update | Yes | Yes | Zip download | Launch | [11](11-quality-and-release.md) |
| Management from the client | Yes | Yes | Channels and roles only | Launch | [07](07-server-management.md) |
| Admin command line / query interface | API | ServerQuery | No | Launch | `partiesctl` |
| One-command server install | — | Yes | Docker image | Launch | Keep; add packages and a Windows service |
| Android client | Yes | Yes | No | Soon | The UI toolkit and core are chosen with this in mind |
| Linux desktop client | Yes | Yes | No | Soon | Mostly capture and audio backends |
| Push notifications on mobile | Yes | Yes | No | Soon | Needs a relay, section 9 |
| Bots and webhooks | Yes | Plugins, ServerQuery | No | Soon | A bot is a headless client with a role; publish the protocol |
| Web client | Yes | — | No | Later | WebTransport makes this possible on the same QUIC port |
| Plugin SDK | — | Yes | No | No | Bots over the protocol instead |
| Paid tiers, boosts | Yes | Licences | — | No | |

## 9. Features that need a shared service

Some expected features cannot be provided by a self-hosted server alone.
Each must be optional, documented, and carry as little data as possible, or
it contradicts "no accounts, no tracking".

| Feature | Why a shared service | Proposed shape |
|---|---|---|
| Mobile push | Apple and Google only accept pushes from the app's publisher | A relay run by the project. Servers send it a wake-up for a device token; the payload has no content, the app fetches from its server after waking. Operators can turn it off |
| Server directory | Somebody has to host the list | Opt-in listing, entries signed by the server key, no user data |
| Join-link landing page | A `parties://` link is useless to someone without the app | A static redirect page that offers the download and then opens the link; the server key stays in the fragment and never reaches the web server |
| Link previews | Fetching from the client reveals the reader's address; fetching from the server is a request-forgery risk | Off by default. When on, the server fetches through a hardened fetcher that refuses private address ranges and limits size and time |
| Update feed | Central by nature | Signed metadata, see [11](11-quality-and-release.md) |

## 10. What to lean into

Parity is the floor. These are the reasons someone picks Parties over the
incumbents, and the rewrite should protect them:

1. **Self-hosted with no account.** One binary, one port, an identity that is
   a key. Nothing to sign up for.
2. **Streaming quality.** AV1 and H.265 hardware encode, per-frame transport
   and the congestion work give low-latency, high-quality screen sharing with
   no paid tier.
3. **Small and fast.** A native client with a small memory footprint, against
   two Electron-class incumbents.
4. **Inspectable.** MIT licence, published protocol, a diagnostics overlay
   that tells the user what the network is doing.
5. **TeamSpeak-grade control with Discord-grade usability.** Granular
   permissions and numeric limits, edited in a client that explains the
   result.

## 11. Recommended launch scope, in one list

1. Roles, overwrites, categories, management UI, audit log.
2. Join codes and links with pinned server keys; join policies; bans, timeouts, identity
   level.
3. All connection-security work in [04](04-connection-security.md), signed
   updates, an honest privacy statement.
4. Voice: push-to-talk on macOS, better voice detection, echo cancellation
   upgrade, server mute/deafen/move, AFK channel, large-room forwarding.
5. Video: camera and multi-stream grid on every desktop platform, role-based
   quality limits.
6. Chat: edit, reply, reactions, mentions and unread state, Markdown, emoji,
   attachments everywhere, typing, notifications, slow mode.
7. Profile: avatar, nickname, presence.
8. iOS: background voice with CallKit, camera, chat attachments.
9. Packaging: Windows installer, Linux packages, Windows service, first-run
   wizard, self-hosting guide.
10. Accessibility and localisation infrastructure.
