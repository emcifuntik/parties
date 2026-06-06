#include <parties/server_query.h>
#include <parties/serialization.h>

#include <cstring>
#include <cstdio>
#include <random>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static const sock_t kInvalidSock = INVALID_SOCKET;
  #define CLOSE_SOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/time.h>
  using sock_t = int;
  static const sock_t kInvalidSock = -1;
  #define CLOSE_SOCK ::close
#endif

namespace parties {

// ── Request ──

std::vector<uint8_t> build_server_query_request(uint32_t token) {
    std::vector<uint8_t> req(SERVER_QUERY_REQUEST_SIZE, 0);
    std::memcpy(req.data(), SERVER_QUERY_MAGIC, sizeof(SERVER_QUERY_MAGIC));
    uint8_t* p = req.data() + SERVER_QUERY_TOKEN_OFFSET;
    p[0] = static_cast<uint8_t>(token & 0xFF);
    p[1] = static_cast<uint8_t>((token >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((token >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((token >> 24) & 0xFF);
    return req;
}

bool is_server_query_request(const uint8_t* data, size_t len) {
    return data != nullptr && len >= sizeof(SERVER_QUERY_MAGIC) &&
        std::memcmp(data, SERVER_QUERY_MAGIC, sizeof(SERVER_QUERY_MAGIC)) == 0;
}

uint32_t server_query_request_token(const uint8_t* data, size_t len) {
    if (data == nullptr || len < SERVER_QUERY_MIN_REQUEST) return 0;
    const uint8_t* p = data + SERVER_QUERY_TOKEN_OFFSET;
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ── Reply ──

std::vector<uint8_t> build_server_query_reply(uint32_t token, const ServerQueryInfo& info) {
    BinaryWriter w;
    w.write_bytes(SERVER_QUERY_REPLY_MARKER, sizeof(SERVER_QUERY_REPLY_MARKER));
    w.write_u32(token);
    w.write_u16(info.protocol_version);
    w.write_u16(info.current_users);
    w.write_u16(info.max_users);
    w.write_u8(info.password_locked ? SERVER_QUERY_FLAG_PASSWORD_LOCKED : 0);

    std::string name = info.server_name;
    if (name.size() > SERVER_QUERY_MAX_NAME) {
        name.resize(SERVER_QUERY_MAX_NAME);
    }
    w.write_string(name);
    return w.data();
}

bool parse_server_query_reply(const uint8_t* data, size_t len,
                              uint32_t expected_token, ServerQueryInfo& out) {
    if (data == nullptr || len < sizeof(SERVER_QUERY_REPLY_MARKER)) return false;
    if (std::memcmp(data, SERVER_QUERY_REPLY_MARKER, sizeof(SERVER_QUERY_REPLY_MARKER)) != 0) {
        return false;
    }

    BinaryReader r(data, len);
    uint8_t marker[sizeof(SERVER_QUERY_REPLY_MARKER)];
    r.read_bytes(marker, sizeof(marker));   // already validated above

    uint32_t token = r.read_u32();
    if (token != expected_token) return false;

    ServerQueryInfo info;
    info.protocol_version = r.read_u16();
    info.current_users    = r.read_u16();
    info.max_users        = r.read_u16();
    uint8_t flags         = r.read_u8();
    info.password_locked  = (flags & SERVER_QUERY_FLAG_PASSWORD_LOCKED) != 0;
    info.server_name      = r.read_string();

    if (r.error()) return false;
    out = std::move(info);
    return true;
}

// ── Client helper ──

std::optional<ServerQueryInfo> query_server(const std::string& host, uint16_t port,
                                             int timeout_ms) {
    char portstr[16];
    std::snprintf(portstr, sizeof(portstr), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;       // IPv4 or IPv6
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || res == nullptr) {
        return std::nullopt;
    }

    std::random_device rd;
    const uint32_t token = static_cast<uint32_t>(rd()) ^ (static_cast<uint32_t>(rd()) << 1);
    const std::vector<uint8_t> request = build_server_query_request(token);

    std::optional<ServerQueryInfo> result;

    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSock) continue;

#if defined(_WIN32)
        DWORD tv = static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        int sent = static_cast<int>(sendto(s, reinterpret_cast<const char*>(request.data()),
                                            static_cast<int>(request.size()), 0,
                                            ai->ai_addr, static_cast<int>(ai->ai_addrlen)));
        if (sent != static_cast<int>(request.size())) {
            CLOSE_SOCK(s);
            continue;
        }

        uint8_t buf[1500];
        int n = static_cast<int>(recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                          nullptr, nullptr));
        CLOSE_SOCK(s);

        if (n > 0) {
            ServerQueryInfo info;
            if (parse_server_query_reply(buf, static_cast<size_t>(n), token, info)) {
                result = std::move(info);
                break;
            }
        }
    }

    freeaddrinfo(res);
    return result;
}

} // namespace parties
