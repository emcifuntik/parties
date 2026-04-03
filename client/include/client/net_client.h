#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
#include <parties/thread_queue.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace parties::client {

// A control message received from the server (wire: [u32 len][u16 type][payload])
struct ServerMessage {
    protocol::ControlMessageType type;
    std::vector<uint8_t>         payload;
};

// Platform-agnostic QUIC client.
//
// Backed by a platform-specific Impl:
//   - net_client_msquic.cpp   (Windows / Linux — MsQuic)
//   - net_client_apple.mm     (macOS / iOS   — Network.framework)
//
// Callers see only this header; no transport headers leak through.
class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&)            = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Start connecting (non-blocking). Poll is_connected() / connect_failed().
    bool connect(const std::string& host, uint16_t port,
                 const uint8_t* ticket = nullptr, size_t ticket_len = 0,
                 bool disable_encryption = false);

    // Disconnect (or cancel a pending connection).
    void disconnect();

    // SHA-256 hex fingerprint of the server's TLS certificate (TOFU).
    // Available after a successful handshake.
    std::string get_server_fingerprint() const;

    // True if the connection has 1-RTT encryption disabled (insecure).
    bool encryption_disabled() const;

    bool is_connected()  const;
    bool is_connecting() const;
    bool connect_failed() const;

    // Send a control message on the reliable stream.
    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    // Send a voice packet as an unreliable datagram.
    bool send_data(const uint8_t* data, size_t len, bool reliable = false);

    // Send a video frame on the reliable video stream.
    bool send_video(const uint8_t* data, size_t len, bool reliable = false);

    // Open the video and datagram streams after authentication.
    // On MsQuic these are opened automatically on connect; on Apple they are
    // deferred until after auth to ensure correct QUIC stream ID ordering.
    void open_av_streams();

    // File transfer — open ephemeral QUIC streams
    bool upload_file(uint64_t attachment_id, const uint8_t* data, size_t len);
    bool download_file(uint64_t attachment_id);

    // Parsed control messages pushed by the transport layer.
    ThreadQueue<ServerMessage>& incoming() { return incoming_; }

    // Callbacks — assign before calling connect().
    std::function<void(const uint8_t* data, size_t len)>   on_data_received;
    std::function<void()>                                   on_disconnected;
    std::function<void(const uint8_t* ticket, size_t len)> on_resumption_ticket;

    // File transfer callbacks
    std::function<void(uint64_t attachment_id, std::vector<uint8_t> data)> on_file_downloaded;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ThreadQueue<ServerMessage> incoming_;  // owned here, written by Impl
};

} // namespace parties::client
