#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

//
// Out-of-RFC connectionless server query ("game-server-browser" style).
//
// A single UDP datagram is sent to the server's QUIC port (default 7800) and a
// single datagram comes back with basic server info, WITHOUT establishing a
// QUIC connection and WITHOUT any encryption. This is served by a custom MsQuic
// patch (vendor/ports/msquic/0001-unconnected-query.patch) that raises a
// QUIC_LISTENER_EVENT_UNCONNECTED_QUERY listener event for datagrams whose first
// 8 bytes match SERVER_QUERY_MAGIC.
//
// Anti-amplification: the server's reply is capped to the request length, so the
// request is padded (SERVER_QUERY_REQUEST_SIZE) to leave room for the reply. The
// reply can therefore never be larger than the request.
//
namespace parties {

//
// First 8 bytes of every query request. MUST stay byte-for-byte identical to
// QuicUnconnectedQueryMagic in the MsQuic patch. Byte 0 (0xC0) sets the QUIC
// long-header form + fixed bit and bytes 1-4 ("PART") are not a real QUIC
// version, so this can never collide with a valid QUIC packet.
//
inline constexpr uint8_t SERVER_QUERY_MAGIC[8] =
    { 0xC0, 'P', 'A', 'R', 'T', 'Y', 'Q', '1' };

// 4-byte marker at the start of every reply, so the client can reject stray UDP.
inline constexpr uint8_t SERVER_QUERY_REPLY_MARKER[4] = { 'P', 'Q', 'R', '1' };

// Request layout: [magic(8)][token(u32 LE)][zero padding ...]
inline constexpr size_t SERVER_QUERY_TOKEN_OFFSET = 8;
inline constexpr size_t SERVER_QUERY_MIN_REQUEST  = 12;   // magic + token

// Padded request size. Large enough to hold any reply (server caps reply <=
// request length for anti-amplification), small enough to stay in one datagram.
inline constexpr size_t SERVER_QUERY_REQUEST_SIZE = 256;

// Server name is clamped to this many bytes in the reply to stay within bounds.
inline constexpr size_t SERVER_QUERY_MAX_NAME = 200;

// Reply flag bits.
inline constexpr uint8_t SERVER_QUERY_FLAG_PASSWORD_LOCKED = 0x01;

// Decoded server info returned by a query.
struct ServerQueryInfo {
    uint16_t    protocol_version = 0;
    std::string server_name;
    uint16_t    current_users  = 0;
    uint16_t    max_users      = 0;
    bool        password_locked = false;
};

// ── Request (client builds, server inspects) ──

// Build a padded query request carrying `token` (echoed back in the reply so the
// client can match the response and ignore stale/spoofed datagrams).
std::vector<uint8_t> build_server_query_request(uint32_t token);

// True if `data` (length `len`) begins with SERVER_QUERY_MAGIC.
bool is_server_query_request(const uint8_t* data, size_t len);

// Extract the token from a received request (0 if the request is too short).
uint32_t server_query_request_token(const uint8_t* data, size_t len);

// ── Reply (server builds, client parses) ──

// Serialize a reply for `info`, echoing the request's `token`.
std::vector<uint8_t> build_server_query_reply(uint32_t token, const ServerQueryInfo& info);

// Parse a reply, verifying the marker and that the echoed token matches
// `expected_token`. Returns false on any mismatch or truncation.
bool parse_server_query_reply(const uint8_t* data, size_t len,
                              uint32_t expected_token, ServerQueryInfo& out);

// ── Client helper ──

// Send a query to `host:port` over UDP and wait up to `timeout_ms` for the
// reply. Returns std::nullopt on resolve/socket error or timeout. Requires
// parties::net_init() to have been called (WSAStartup on Windows).
std::optional<ServerQueryInfo> query_server(const std::string& host, uint16_t port,
                                             int timeout_ms = 1000);

} // namespace parties
