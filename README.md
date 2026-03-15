# Parties

Self-hosted voice chat and screen sharing app. No accounts, no tracking — just connect and talk.

![Parties screenshot](images/streams.png)

## Features

- **Voice chat** with Opus codec, noise cancellation (RNNoise), and echo cancellation
- **Screen sharing** with hardware-accelerated encoding (AV1/H.265/H.264)
- **End-to-end encryption** via QUIC (TLS 1.3)
- **Ed25519 identity** — no passwords, no email, just a seed phrase
- **SFU architecture** — server forwards, never decodes
- **Self-hosted** — run your own server on any machine

## Building

### Prerequisites

- CMake 3.25+
- Clang/LLVM 20+ (clang-cl on Windows)
- vcpkg (manifest mode, auto-bootstrapped)
- Ninja

### Build

```bash
cmake --preset default
cmake --build --preset default
```

### Presets

| Preset | Description |
|--------|-------------|
| `default` | Debug build |
| `release` | Optimized release build |
| `asan` | RelWithDebInfo + AddressSanitizer |

## Architecture

Single QUIC connection on UDP port 7800:

- **Control stream** (stream 0) — bidirectional, length-prefixed messages
- **Video stream** (stream 1) — reliable screen share frames
- **Voice datagrams** — unreliable, unordered Opus packets

See [docs/protocol.md](docs/protocol.md) for the full protocol specification.

## License

This project is licensed under the [MIT License](LICENSE).

All dependencies (OpenSSL, MsQuic, Opus, RNNoise, etc.) use MIT/Apache/BSD-compatible licenses.
