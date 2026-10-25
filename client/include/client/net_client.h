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

struct ServerMessage {
    protocol::ControlMessageType type;
    std::vector<uint8_t>         payload;
};

class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&)            = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const std::string& host, uint16_t port,
                 const uint8_t* ticket = nullptr, size_t ticket_len = 0);

    void disconnect();

    std::string get_server_fingerprint() const;

    bool is_connected()  const;
    bool is_connecting() const;
    bool connect_failed() const;

    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    // Send a voice/control packet as an unreliable datagram. `priority` puts it
    // ahead of other queued datagrams (QUIC_SEND_FLAG_DGRAM_PRIORITY) — use it
    // for voice so video never delays it.
    bool send_data(const uint8_t* data, size_t len, bool priority = false);

    bool send_video(const uint8_t* data, size_t len);

    bool send_video_parts(const uint8_t* header, size_t header_len,
                          const uint8_t* payload, size_t payload_len);

    bool send_video_frame_stream(const uint8_t* header, size_t header_len,
                                 const uint8_t* payload, size_t payload_len);

    struct ConnectionStats {
        bool     valid = false;
        uint32_t rtt_us = 0;
        uint32_t min_rtt_us = 0;
        uint64_t sent_packets = 0;
        uint64_t suspected_lost_packets = 0;
        uint64_t spurious_lost_packets = 0;
        uint64_t sent_bytes = 0;
        uint32_t congestion_window = 0;
    };
    ConnectionStats connection_stats() const;

    bool upload_file(uint64_t attachment_id, const uint8_t* data, size_t len);
    bool download_file(uint64_t attachment_id);

    ThreadQueue<ServerMessage>& incoming() { return incoming_; }

    std::function<void(const uint8_t* data, size_t len)>   on_data_received;
    std::function<void()>                                   on_disconnected;
    std::function<void(const uint8_t* ticket, size_t len)> on_resumption_ticket;

    std::function<void(size_t bytes)>                       on_video_bytes_completed;

    std::function<void(uint32_t sender_id, std::vector<uint8_t>&& frame)> on_video_stream_frame;
    std::function<void(uint32_t sender_id, uint32_t frame_seq, bool have_seq)> on_video_stream_aborted;

    std::function<void(uint64_t attachment_id, std::vector<uint8_t> data)> on_file_downloaded;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ThreadQueue<ServerMessage> incoming_;
};

}
