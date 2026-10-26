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

ARG VCPKG_COMMIT=aa2d37682e3318d93aef87efa7b0e88e81cd3d59
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && cd /opt/vcpkg \
    && git checkout ${VCPKG_COMMIT} \
    && ./bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src
COPY . .

RUN ln -s /opt/vcpkg /src/vcpkg

ARG SENTRY_DSN_SERVER=""

# Override CMAKE_MAKE_PROGRAM: the preset points at vendor/ninja/ninja-linux
# which loses its execute bit during COPY; use the system ninja instead.
RUN cmake --preset linux-release \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DENABLE_SENTRY=$([ -n "$SENTRY_DSN_SERVER" ] && echo "ON" || echo "OFF") \
    -DSENTRY_DSN_SERVER="$SENTRY_DSN_SERVER"

RUN cmake --build build-linux --config Release

FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build-linux/server/parties_server /usr/local/bin/parties_server

EXPOSE 7800/udp

VOLUME /data
WORKDIR /data

ENTRYPOINT ["parties_server"]
