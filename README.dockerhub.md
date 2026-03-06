# Parties Server

Self-hosted voice, video, and text communication server. Uses QUIC (UDP) for all traffic on a single port — low latency voice chat with Ed25519 identity-based authentication.

## Quick Start

```bash
docker run -d \
  --name parties \
  -p 7800:7800/udp \
  -v parties-data:/data \
  -e PARTIES_SERVER_NAME="My Server" \
  tuxick/parties-server:latest
```

Or with Docker Compose:

```yaml
services:
  parties-server:
    image: tuxick/parties-server:latest
    ports:
      - "7800:7800/udp"
    volumes:
      - parties-data:/data
    environment:
      PARTIES_SERVER_NAME: "My Server"
      PARTIES_ROOT_FINGERPRINTS: "your-ed25519-fingerprint-here"
    restart: unless-stopped

volumes:
  parties-data:
```

## Configuration

Configure via environment variables, a `server.toml` file in `/data`, or both. Environment variables take priority over the TOML file.

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `PARTIES_SERVER_NAME` | Server display name | `Parties Server` |
| `PARTIES_LISTEN_IP` | Bind address | `0.0.0.0` |
| `PARTIES_PORT` | QUIC UDP port | `7800` |
| `PARTIES_MAX_CLIENTS` | Max concurrent connections | `64` |
| `PARTIES_PASSWORD` | Server password (empty = no password) | — |
| `PARTIES_CERT_FILE` | TLS certificate path (relative to `/data`) | `server.pem` |
| `PARTIES_KEY_FILE` | TLS private key path (relative to `/data`) | `server.key.pem` |
| `PARTIES_DB_PATH` | SQLite database path | `parties.db` |
| `PARTIES_ROOT_FINGERPRINTS` | Comma-separated Ed25519 fingerprints for Owner role | — |
| `PARTIES_MAX_USERS_PER_CHANNEL` | Max users per voice channel | `32` |
| `PARTIES_DEFAULT_BITRATE` | Opus bitrate in bps | `32000` |
| `PARTIES_LOG_LEVEL` | `debug`, `info`, or `warn` | `info` |

### TOML Configuration

Mount a `server.toml` into `/data` for file-based config:

```toml
[server]
name = "My Server"
listen_ip = "0.0.0.0"
port = 7800
max_clients = 64
# password = "optional-server-password"

[tls]
cert_file = "server.pem"
key_file = "server.key.pem"

[database]
path = "parties.db"

[identity]
root_fingerprints = ["your-ed25519-fingerprint"]

[voice]
max_users_per_channel = 32
default_bitrate = 32000

[logging]
level = "info"
```

## TLS Certificates

The server generates a self-signed certificate on first run if no cert files are found. To use your own:

```bash
docker run -d \
  -p 7800:7800/udp \
  -v parties-data:/data \
  -v /path/to/certs:/certs:ro \
  -e PARTIES_CERT_FILE=/certs/server.pem \
  -e PARTIES_KEY_FILE=/certs/server.key.pem \
  tuxick/parties-server:latest
```

## Volumes

| Path | Purpose |
|------|---------|
| `/data` | Working directory — stores `server.toml`, certificates, and `parties.db` |

## Networking

The server uses a single UDP port (default 7800) for everything: QUIC control plane, voice, and video. Make sure your firewall and Docker port mapping use UDP:

```bash
-p 7800:7800/udp
```

## Owner / Root Access

Set `PARTIES_ROOT_FINGERPRINTS` to a comma-separated list of Ed25519 identity fingerprints. Users with matching fingerprints get Owner (root) privileges — full server administration including channel management and user moderation.

Your fingerprint is shown in the Parties client under your identity settings.

## Source Code

[github.com/emcifuntik/miniaudio-rnnoise](https://github.com/emcifuntik/miniaudio-rnnoise)
