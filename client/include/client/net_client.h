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
                 const uint8_t* ticket = nullptr, size_t ticket_len = 0);

    // Disconnect (or cancel a pending connection).
    void disconnect();

    // SHA-256 hex fingerprint of the server's TLS certificate (TOFU).
    // Available after a successful handshake.
    std::string get_server_fingerprint() const;

    bool is_connected()  const;
    bool is_connecting() const;
    bool connect_failed() const;

    // Send a control message on the reliable stream.
    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    // Send a voice/control packet as an unreliable datagram. `priority` puts it
    // ahead of other queued datagrams (QUIC_SEND_FLAG_DGRAM_PRIORITY) — use it
    // for voice so video never delays it.
    bool send_data(const uint8_t* data, size_t len, bool priority = false);

    // Send a whole packet on the legacy reliable video stream (stream 1). Used
    // for PLI (reliable, ordered) and for video toward pre-1.2 servers.
    // Video bytes sent here are reported through on_video_bytes_completed.
    bool send_video(const uint8_t* data, size_t len, bool reliable = false);

    // Scatter/gather-friendly producer API for stream 1. The transport owns one
    // contiguous copy for MsQuic's asynchronous send lifetime, avoiding an
    // intermediate application packet and a second full bitstream copy.
    bool send_video_parts(const uint8_t* header, size_t header_len,
                          const uint8_t* payload, size_t payload_len);

    // Per-frame video stream (servers >= 1.2). Opens one UNIDIRECTIONAL QUIC
    // stream, sends [STREAM_TYPE_VIDEO_FRAME][header][payload] with FIN in one
    // StreamSend and closes it on SHUTDOWN_COMPLETE. Returns false (and queues
    // nothing) if the stream could not be opened/started/sent — the caller
    // treats that as a lost frame. Completed bytes are reported through
    // on_video_bytes_completed exactly like stream-1 video.
    bool send_video_frame_stream(const uint8_t* header, size_t header_len,
                                 const uint8_t* payload, size_t payload_len);

    // Snapshot of QUIC_PARAM_CONN_STATISTICS_V2 for congestion control.
    // valid == false when not connected.
    struct ConnectionStats {
        bool     valid = false;
        uint32_t rtt_us = 0;
        uint32_t min_rtt_us = 0;
        uint64_t sent_packets = 0;            // SendTotalPackets
        uint64_t suspected_lost_packets = 0;  // SendSuspectedLostPackets
        uint64_t spurious_lost_packets = 0;   // SendSpuriousLostPackets
        uint64_t sent_bytes = 0;              // SendTotalBytes
        uint32_t congestion_window = 0;       // SendCongestionWindow
    };
    ConnectionStats connection_stats() const;

    // Open the video and datagram streams after authentication.
    // On MsQuic these are opened automatically on connect; on Apple they are
    // deferred until after auth to ensure correct QUIC stream ID ordering.
    void open_av_streams();

    // File transfer — open ephemeral QUIC streams
    bool upload_file(uint64_t attachment_id, const uint8_t* data, size_t len);
    bool download_file(uint64_t attachment_id);

    // Parsed control messages pushed by the transport layer.
    ThreadQueue<ServerMessage>& incoming() { return incoming_; }

    // Callbacks — assign before calling connect(). All fire on the MsQuic
    // worker thread unless noted.
    std::function<void(const uint8_t* data, size_t len)>   on_data_received;
    std::function<void()>                                   on_disconnected;
    std::function<void(const uint8_t* ticket, size_t len)> on_resumption_ticket;

    // Video send accounting: bytes of video (stream 1 or per-frame streams)
    // that MsQuic has finished with (acknowledged). Paired with the byte counts
    // the caller added after each successful send call.
    std::function<void(size_t bytes)>                       on_video_bytes_completed;

    // A complete frame arrived on a server-opened per-frame stream.
    // `frame` = [14-byte VideoFrameHeader][encoded] — the same layout that
    // on_data_received sees after the [type][sender] prefix on stream 1.
    std::function<void(uint32_t sender_id, std::vector<uint8_t>&& frame)> on_video_stream_frame;
    // A per-frame stream was aborted by the server before completing. frame_seq
    // is known only when at least the header had arrived; otherwise it is
    // reported with have_seq == false.
    std::function<void(uint32_t sender_id, uint32_t frame_seq, bool have_seq)> on_video_stream_aborted;

    // File transfer callbacks
    std::function<void(uint64_t attachment_id, std::vector<uint8_t> data)> on_file_downloaded;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ThreadQueue<ServerMessage> incoming_;  // owned here, written by Impl
};

} // namespace parties::client
