#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
#include <parties/thread_queue.h>
#include <parties/quic_common.h>

#include <string>
#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <unordered_map>

namespace parties::client {

// A control message received from the server
struct ServerMessage {
    protocol::ControlMessageType type;
    std::vector<uint8_t>         payload;
};

class NetClient {
public:
    NetClient();
    ~NetClient();

    // Start connecting to server via QUIC (non-blocking).
    // Returns false if setup failed immediately (bad params, MsQuic not init).
    // Poll is_connected() / connect_failed() to check result.
    bool connect(const std::string& host, uint16_t port,
                 const uint8_t* ticket = nullptr, size_t ticket_len = 0);

    // Get the server certificate fingerprint (after QUIC connect)
    std::string get_server_fingerprint() const;

    // Disconnect from server (or cancel pending connection)
    void disconnect();

    bool is_connected() const { return connected_; }
    bool is_connecting() const { return connecting_ && !connected_ && !connect_failed_; }
    bool connect_failed() const { return connect_failed_; }

    // Send a control message to the server (reliable, on control stream)
    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    // Send a data packet (unreliable datagram — voice)
    bool send_data(const uint8_t* data, size_t len, bool reliable = false);

    // Send a video packet (reliable, on video stream)
    bool send_video(const uint8_t* data, size_t len, bool reliable = false);

    // Queue of messages from the server (control plane)
    ThreadQueue<ServerMessage>& incoming() { return incoming_; }

    // Callback for data packets received (datagrams — voice/video)
    std::function<void(const uint8_t* data, size_t len)> on_data_received;

    // Callback for disconnect events
    std::function<void()> on_disconnected;

    // Callback for resumption ticket (save for 0-RTT reconnection)
    std::function<void(const uint8_t* ticket, size_t len)> on_resumption_ticket;

private:
    // MsQuic callbacks
    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection, void* context,
                                                     QUIC_CONNECTION_EVENT* event);
    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream, void* context,
                                                 QUIC_STREAM_EVENT* event);

    QUIC_STATUS on_connection_event(HQUIC connection, QUIC_CONNECTION_EVENT* event);
    QUIC_STATUS on_stream_event(HQUIC stream, QUIC_STREAM_EVENT* event);

    // Process received stream data (accumulate and parse length-prefixed messages)
    void process_stream_data(const uint8_t* data, size_t len);

    // Process received video stream data (length-prefixed video packets)
    void process_video_stream_data(const uint8_t* data, size_t len);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC connection_ = nullptr;
    HQUIC control_stream_ = nullptr;
    HQUIC video_stream_ = nullptr;

    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    std::atomic<bool> connect_failed_{false};
    std::mutex write_mutex_;

    // Stream receive buffers
    std::mutex buffer_mutex_;
    std::vector<uint8_t> recv_buffer_;
    std::mutex video_buffer_mutex_;
    std::vector<uint8_t> video_recv_buffer_;

    ThreadQueue<ServerMessage> incoming_;
    std::string server_fingerprint_;
};

} // namespace parties::client
