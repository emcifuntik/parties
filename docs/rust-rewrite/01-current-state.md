# 01 — Current state of the C++ system

> Status: Draft — 2026-11-10. Inventory taken at commit `df60746`
> (`feature/diagnostics-overlay`). File and line references are to that commit.

This document records what exists today, so the rewrite has an explicit parity
target and an explicit list of things it is allowed to leave behind.

## 1. System at a glance

Parties is a self-hosted voice, video and text chat system. A client holds one
QUIC connection (UDP 7800) to one server. The server is a selective forwarding
unit (SFU): it authenticates users, keeps channel state in SQLite and forwards
media without decoding it.

| Component | Path | Approx. own code | Role |
|---|---|---|---|
| Shared library | `common/` | 4.5k lines | Protocol constants, serialization, crypto, logging, video transport logic (send controller, reorder, backlog, playout), server query |
| Server | `server/` | 5.8k lines | MsQuic listener, session and channel state, SFU, chat, file transfer, SQLite |
| Client core | `client/src/*.cpp`, `client/include/` | 13k lines | `AppCore` (4k lines), models, audio engine, mixer, network client, settings |
| Client platform shells | `client/src/windows`, `macos`, `ios`, `metal`, `dx12` | 14k lines | Windowing, rendering glue, capture, tray, updater |
| Codec library | `encdec/` | 6k lines + 6.6k vendored NVIDIA headers | Hardware and software video encode/decode |
| UI documents | `client/ui/` | 7.4k lines RML/RCSS | Two RmlUi documents and the stylesheet cascade |
| Designer | `designer/` | 3.4k lines | Live-reload UI designer and screenshot tool (Windows only) |
| Tests | `tests/` | 10.6k lines | 39 test programs |

Vendored third-party code inside the tree (not counted above): the RmlUi
Win32/DX12 backend with D3D12MemAlloc (about 24k lines) and an unused
`stb_truetype.h`.

## 2. Third-party dependencies

| Dependency | Use | Platforms |
|---|---|---|
| MsQuic 2.5.6, four overlay patches (unconnected query, datapath offload toggle, two BBR changes) | All transport | All |
| OpenSSL | Ed25519, SHA-256, RNG, self-signed RSA certificate | All |
| Opus | Voice, secondary voice, stream audio | All |
| miniaudio | Audio device I/O | Clients |
| RNNoise | Noise suppression | Clients |
| speexdsp | Echo cancellation (MDF) | Clients |
| RmlUi 6.3 (+ FreeType, SVG) | UI | Clients |
| dav1d | AV1 software decode | Clients |
| libhevc, openh264 | H.265 / H.264 software paths | Windows |
| AMD AMF headers, NVIDIA Video Codec SDK headers | Hardware encode/decode | Windows |
| SQLite (FTS5) | Client settings, server database, chat search | All |
| spdlog, Tracy, sentry-native / sentry-cocoa | Logging, profiling, crash reports | Varies |
| Sparkle 2.9.1 | macOS updates | macOS |
| toml11, rapidjson | Server config, updater JSON | Server, Windows |

## 3. Capability matrix

| Capability | Windows | macOS | iOS |
|---|---|---|---|
| Audio I/O | miniaudio (WASAPI) | miniaudio (CoreAudio) | miniaudio (CoreAudio), UIKit-owned `AVAudioSession` |
| Noise suppression, echo cancellation, VAD | RNNoise, speexdsp, RMS gate | Same | Same |
| Push-to-talk and global hotkeys | `GetAsyncKeyState` polling | Not implemented | Not available |
| Opus | 40 kbps VOIP, in-band FEC, no DTX; 64 kbps music profile for the secondary stream | Same | Same |
| Jitter buffer and mixer | `VoiceMixer`: priming, FEC, PLC, per-user volume; separate aux mixer | Same | Same |
| Screen capture | Windows.Graphics.Capture, D3D11 | ScreenCaptureKit, system picker | Not implemented |
| Window picker with thumbnails | Yes | System picker only | n/a |
| Stream audio | WASAPI process loopback | ScreenCaptureKit audio | n/a |
| Camera | Media Foundation | Not implemented | Not implemented |
| Video encode | NVENC, AMF, MFT, OpenH264; AV1 > H.265 > H.264 | VideoToolbox H.264/H.265 | None |
| Video decode | NVDEC, AMF, MFT, dav1d, libhevc, OpenH264 | VideoToolbox, dav1d | VideoToolbox, dav1d |
| Multi-stream grid | Yes | Single stream | Single stream |
| Rendering | RmlUi DX12 backend, `Dx12VideoConverter` | Metal renderer, NV12 pipeline | Metal renderer |
| Context menus | RmlUi popup windows | `NSMenu`, `NSPopover` | Action sheet |
| Tray / status item | Yes | Yes | n/a |
| Single instance, sleep inhibit | Yes, yes | No, yes | n/a, yes |
| Auto-update | GitHub release zip, Authenticode check | Sparkle (EdDSA) | None |
| Chat attachments (pick/save) | Yes | No-op | No-op |
| Diagnostics overlay | Yes, with hardware snapshot | Yes | Yes |
| Auto-reconnect, server browser, identity UX | Yes | Yes | Yes |
| CallKit, push notifications | n/a | n/a | None |
| Localisation | None | None | None |

The Apple clients are behind Windows in six areas: hotkeys and push-to-talk,
camera, multi-stream viewing, AV1 encode, chat attachments, and the in-app
share picker. The rewrite plans these as first-class work, not as ports.

## 4. Protocol 1.4 in brief

- Control: one bidirectional stream, `[u32 len][u16 type][payload]`,
  hand-written little-endian serialization, about 45 message types.
- Voice and stream audio: QUIC datagrams, `[type][opus]` in,
  `[type][sender u32][opus]` out.
- Video: one unidirectional stream per encoded frame (since 1.2), with a
  14-byte header; a legacy bidirectional stream for pre-1.2 peers and PLIs.
- File transfer: extra bidirectional streams, positional (third and later).
- Server browser: one unencrypted UDP datagram answered by a patched MsQuic.
- Roles: four fixed roles and a 15-bit permission mask.

`docs/protocol.md` is the reference for 1.4. Sections 10 and 11 of that file
are stale: they still describe wolfSSL, a ±60 s auth window, `CHANNEL_KEY`,
0-RTT and skip-auth resumption. None of that matches the code. See
[04-connection-security.md](04-connection-security.md).

## 5. Threading today

Server: MsQuic worker threads push into thread-safe queues; one main loop
drains them every millisecond and does all state changes and synchronous
SQLite work. Several mutexes guard state the workers read directly.

Windows client: a message thread runs `AppCore::tick()`, a render thread owns
the swap chain, and both share the RmlUi context through a recursive mutex.
Around a dozen further threads handle audio callbacks, capture, encode, one
decode thread per stream, the camera, the server browser and the updater.
Cross-thread hand-off is a mix of queues, atomics and guarded buffers. DSP and
Opus encoding run inside the audio capture callback.

Apple clients: a 60 Hz main-thread tick; capture and codec callbacks hop back
with `dispatch_async`.

## 6. Build, release and tests

- Build: CMake presets, vcpkg manifest mode with overlay ports, clang-cl on
  Windows, Xcode toolchains on Apple, GCC in Docker for the Linux server.
- CI: Windows and macOS build only; Linux builds the server and runs the
  integration test. No iOS job. `ctest` is not run in CI.
- Release: Windows zips signed through a remote signing service; macOS DMG,
  notarised, with a Sparkle appcast; server image on GHCR. iOS archives are
  manual.
- Tests: about half are pure logic (video transport, datagram backlog, mixer,
  diagnostics, hotkey combos); the rest need a GPU, a codec or an OS service.
  Three network tests run real QUIC over loopback, two of them through a UDP
  impairment relay (`tests/net_impairment.h`).

## 7. What must survive the rewrite

These are the hard-won parts. Each one is ported as behaviour, with its tests,
not redesigned.

| Asset | Where | Why it matters |
|---|---|---|
| Video send controller (admission + AIMD) | `common/src/video_send_controller.cpp` | Root-cause fix for "slow motion" video |
| Per-viewer backlog gate and keyframe discipline | `common/include/parties/video_backlog.h`, `server/src/server.cpp` | A slow viewer never hurts others |
| Frame reorder buffer and receive router | `common/src/video_frame_reorder.cpp`, `video_receive_router.cpp` | PLI pacing, gap handling |
| Playout clock | `common/src/video_playout.cpp` | Smooth playback under jitter |
| Datagram backlog policy | `common/include/parties/datagram_backlog.h` | Voice stays live when a path collapses |
| Voice mixer and jitter buffer | `client/src/voice_mixer.cpp` | FEC, PLC and resync behaviour users are used to |
| Diagnostics recorder and freeze classifier | `client/src/diagnostics.cpp` | Field debugging |
| Auto-reconnect policy | `client/src/app_core.cpp` | Backoff, flap guard, channel rejoin |
| Impairment test scenarios | `tests/voice_congestion_test.cpp`, `video_network_test.cpp` | Acceptance tests for any new transport |
| Design tokens, type scale, layouts | `client/ui/theme.rcss`, `typography.rcss`, `lobby.rml` | The visual design and the typography contract in `AGENTS.md` |
| Release pipeline knowledge | `.github/workflows/release.yml` | Signing, notarisation, appcast validation |

## 8. What the rewrite deliberately leaves behind

| Item | Reason |
|---|---|
| Protocol 1.x wire compatibility | The auth handshake and the permission model change in incompatible ways (see [03](03-protocol-v2.md), [04](04-connection-security.md)) |
| Four fixed roles | Replaced by the role system in [05](05-permissions-and-roles.md) |
| Positional stream roles, legacy video stream 1 | Every stream is typed in v2 |
| MsQuic and its four patches | See decision D-02 in [12-decisions.md](12-decisions.md) |
| `AppCore` god object, recursive UI mutex, polled 1 ms server loop | Replaced by the structure in [02](02-target-architecture.md) |
| Hand-written serialization | Schema-driven control messages |
| Plaintext secrets in the client database | OS keystore |
| Separate voice-channel and text-channel tables | One channel tree with categories |

## 9. Known defects that the rewrite must not port

The connection-security review found 28 issues; two were critical and reachable
without authentication or by any server operator. They are listed with
evidence in [04-connection-security.md](04-connection-security.md); the two
critical ones have since been fixed in the C++ code as protocol 1.5. The older
backlog in `docs/analysis-backlog.md` still has open items (S6, S7, S8, R2,
R5, E1–E4, X1–X3) that the new design closes by construction.
