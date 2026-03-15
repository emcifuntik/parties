# ── Builder stage ──────────────────────────────────────────────
# Debian trixie ships gcc 14 which has full std::format support
# (needed by spdlog with SPDLOG_USE_STD_FORMAT / default-features=false)
FROM debian:trixie AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    git curl zip unzip tar \
    autoconf automake libtool \
    python3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Clone vcpkg at the same commit as the project submodule
ARG VCPKG_COMMIT=aa2d37682e3318d93aef87efa7b0e88e81cd3d59
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && cd /opt/vcpkg \
    && git checkout ${VCPKG_COMMIT} \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Copy project source (vcpkg submodule excluded via .dockerignore)
WORKDIR /src
COPY . .

# Symlink vcpkg into the source tree so CMakePresets paths resolve
RUN ln -s /opt/vcpkg /src/vcpkg

# Sentry DSN (optional — passed via --build-arg)
ARG SENTRY_DSN_SERVER=""

# Configure (vcpkg install + cmake generate)
# Override CMAKE_MAKE_PROGRAM: the preset points at vendor/ninja/ninja-linux
# which loses its execute bit during COPY; use the system ninja instead.
RUN cmake --preset linux-release \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DENABLE_SENTRY=$([ -n "$SENTRY_DSN_SERVER" ] && echo "ON" || echo "OFF") \
    -DSENTRY_DSN_SERVER="$SENTRY_DSN_SERVER"

# Build (compile + link)
RUN cmake --build build-linux --config Release

# ── Runtime stage ─────────────────────────────────────────────
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build-linux/server/parties_server /usr/local/bin/parties_server

# QUIC (UDP) control + data plane on single port
EXPOSE 7800/udp

# /data holds server.toml (optional), certs, and the SQLite database
VOLUME /data
WORKDIR /data

# ── Configuration via environment variables ───────────────────
# All optional — env vars override server.toml which overrides defaults.
#
#   PARTIES_SERVER_NAME          Server display name         (default: "Parties Server")
#   PARTIES_LISTEN_IP            Bind address                (default: "0.0.0.0")
#   PARTIES_PORT                 QUIC UDP port               (default: 7800)
#   PARTIES_MAX_CLIENTS          Max concurrent connections  (default: 64)
#   PARTIES_PASSWORD             Server password             (default: none)
#   PARTIES_CERT_FILE            TLS certificate path        (default: "server.pem")
#   PARTIES_KEY_FILE             TLS private key path        (default: "server.key.pem")
#   PARTIES_DB_PATH              SQLite database path        (default: "parties.db")
#   PARTIES_ROOT_FINGERPRINTS    Comma-separated Ed25519 fingerprints for Owner role
#   PARTIES_MAX_USERS_PER_CHANNEL                            (default: 32)
#   PARTIES_DEFAULT_BITRATE      Opus bitrate in bps         (default: 32000)
#   PARTIES_LOG_LEVEL            "debug", "info", "warn"     (default: "info")

ENTRYPOINT ["parties_server"]
