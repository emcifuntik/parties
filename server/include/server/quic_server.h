#pragma once

#include <server/session.h>
#include <parties/thread_queue.h>
#include <parties/protocol.h>
#include <parties/quic_common.h>

#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <unordered_map>

namespace parties::server {

// A control message received from a client (same as TlsServer's IncomingMessage)
struct IncomingMessage {
    uint32_t                      session_id;
    protocol::ControlMessageType  type;
    std::vector<uint8_t>          payload;
};

// A data packet received from a client (same as EnetServer's DataPacket)
struct DataPacket {
    uint32_t    session_id;
    uint8_t     packet_type;   // VOICE_PACKET_TYPE etc.
    uint8_t     channel_id;    // Unused in QUIC (kept for compatibility)
    bool        reliable;      // Datagrams = unreliable, streams = reliable
    std::vector<uint8_t> data;
};

class QuicServer {
public:
    QuicServer();
    ~QuicServer();

    // Start the QUIC listener
    bool start(const std::string& listen_ip, uint16_t port, size_t max_clients,
               const std::string& cert_file, const std::string& key_file);

    // Stop the server
    void stop();

    // ── Control plane (replaces TlsServer) ──

    // Queue of control messages received from clients
    ThreadQueue<IncomingMessage>& incoming() { return control_incoming_; }

    // Send a control message to a specific session
    bool send_to(uint32_t session_id, protocol::ControlMessageType type,
                 const uint8_t* payload, size_t payload_len);

    // Broadcast to all authenticated sessions
    void broadcast(protocol::ControlMessageType type,
                   const uint8_t* payload, size_t payload_len);

    // Disconnect a session
    void disconnect(uint32_t session_id);

    // Get a session (may return nullptr)
    std::shared_ptr<Session> get_session(uint32_t session_id);

    // Get all sessions (snapshot)
    std::vector<std::shared_ptr<Session>> get_sessions();

    // Callback for when a session disconnects
    std::function<void(uint32_t session_id)> on_disconnect;

    // Callback for video frames — called directly from QUIC receive thread
    // to bypass the polling loop and avoid 1ms+ latency.
    // Parameters: session_id, packet_type, data (after type byte), length
    std::function<void(uint32_t session_id, uint8_t packet_type,
                       const uint8_t* data, size_t len)> on_video_frame;

    // ── Data plane (replaces EnetServer) ──

    // Queue of received data packets (voice, video)
    ThreadQueue<DataPacket>& data_incoming() { return data_incoming_; }

    // Send a datagram to a specific peer (voice)
    bool send_datagram(uint32_t session_id, const uint8_t* data, size_t len);

    // Send a datagram to all peers in a list (SFU voice fan-out)
    void send_to_many(const std::vector<uint32_t>& session_ids,
                      const uint8_t* data, size_t len);

    // Send a length-prefixed video frame on the session's video stream
    bool send_video_to(uint32_t session_id, const uint8_t* data, size_t len);

    // Send on a reliable stream to a specific peer (video)
    bool send_stream(uint32_t session_id, const uint8_t* data, size_t len);

    // Send to many — for video, uses datagrams (same as voice for now)
    void send_to_many_on_channel(const std::vector<uint32_t>& session_ids,
                                 uint8_t channel, const uint8_t* data, size_t len,
                                 uint32_t flags);

    // Compatibility — same as send_datagram
    bool send_to_on_channel(uint32_t session_id, uint8_t channel,
                            const uint8_t* data, size_t len, uint32_t flags);

private:
    // MsQuic callback functions (static, forward to member via context)
    static QUIC_STATUS QUIC_API listener_callback(HQUIC listener, void* context,
                                                   QUIC_LISTENER_EVENT* event);
    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection, void* context,
                                                     QUIC_CONNECTION_EVENT* event);
    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream, void* context,
                                                 QUIC_STREAM_EVENT* event);

    // Internal event handlers
    QUIC_STATUS on_new_connection(HQUIC listener, QUIC_LISTENER_EVENT* event);
    QUIC_STATUS on_connection_event(HQUIC connection, uint32_t session_id,
                                    QUIC_CONNECTION_EVENT* event);
    QUIC_STATUS on_stream_event(HQUIC stream, uint32_t session_id,
                                QUIC_STREAM_EVENT* event);

    // Send length-prefixed control message on a session's control stream
    bool send_control_on_stream(HQUIC stream, protocol::ControlMessageType type,
                                const uint8_t* payload, size_t payload_len);

    // Process received stream data (accumulate and parse length-prefixed messages)
    void process_stream_data(uint32_t session_id, const uint8_t* data, size_t len);

    // Process received video stream data (length-prefixed video packets)
    void process_video_stream_data(uint32_t session_id, const uint8_t* data, size_t len);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::atomic<bool> running_{false};

    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
    uint32_t next_session_id_ = 1;

    // Per-session stream receive buffers
    std::mutex buffers_mutex_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> recv_buffers_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> video_recv_buffers_;

    ThreadQueue<IncomingMessage> control_incoming_;
    ThreadQueue<DataPacket> data_incoming_;
};

} // namespace parties::server
