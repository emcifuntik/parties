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

struct IncomingMessage {
    uint32_t                      session_id;
    protocol::ControlMessageType  type;
    std::vector<uint8_t>          payload;
};

struct DataPacket {
    uint32_t    session_id;
    uint8_t     packet_type;
    uint8_t     channel_id;
    bool        reliable;
    std::vector<uint8_t> data;
};

struct FileUploadEvent {
    uint32_t session_id;
    uint64_t attachment_id;
    std::vector<uint8_t> data;
};

struct FileDownloadRequest {
    uint32_t session_id;
    uint64_t attachment_id;
    HQUIC stream;
};

struct SessionDisconnect {
    uint32_t  session_id;
    UserId    user_id;
    ChannelId channel_id;
};

class QuicServer {
public:
    QuicServer();
    ~QuicServer();

    bool start(const std::string& listen_ip, uint16_t port, size_t max_clients,
               const std::string& cert_file, const std::string& key_file);

    void stop();

    ThreadQueue<IncomingMessage>& incoming() { return control_incoming_; }

    bool send_to(uint32_t session_id, protocol::ControlMessageType type,
                 const uint8_t* payload, size_t payload_len);

    void broadcast(protocol::ControlMessageType type,
                   const uint8_t* payload, size_t payload_len);

    void disconnect(uint32_t session_id);

    std::shared_ptr<Session> get_session(uint32_t session_id);

    std::vector<std::shared_ptr<Session>> get_sessions();


    void set_server_info(std::string name, uint16_t max_users, bool password_locked);

    ThreadQueue<SessionDisconnect>& disconnects() { return disconnects_; }

    std::function<void(uint32_t session_id, uint8_t packet_type,
                       const uint8_t* data, size_t len)> on_video_frame;

    ThreadQueue<DataPacket>& data_incoming() { return data_incoming_; }

    bool send_datagram(uint32_t session_id, const uint8_t* data, size_t len,
                       bool priority = false);

    void send_to_many(const std::vector<uint32_t>& session_ids,
                      const uint8_t* data, size_t len, bool priority = false);

    bool send_video_to(uint32_t session_id, const uint8_t* data, size_t len);

    bool send_video_frame_to(const std::shared_ptr<Session>& session,
                             std::shared_ptr<const std::vector<uint8_t>> frame);

    bool send_stream(uint32_t session_id, const uint8_t* data, size_t len);

    void send_to_many_on_channel(const std::vector<uint32_t>& session_ids,
                                 uint8_t channel, const uint8_t* data, size_t len,
                                 uint32_t flags);

    bool send_to_on_channel(uint32_t session_id, uint8_t channel,
                            const uint8_t* data, size_t len, uint32_t flags);

    ThreadQueue<FileUploadEvent>& file_uploads() { return file_uploads_; }
    ThreadQueue<FileDownloadRequest>& file_downloads() { return file_downloads_; }

    bool send_file_on_stream(HQUIC stream, const uint8_t* data, size_t len);

    uint64_t reliable_send_failures() const {
        return reliable_send_failures_.load(std::memory_order_relaxed);
    }

private:
    static QUIC_STATUS QUIC_API listener_callback(HQUIC listener, void* context,
                                                   QUIC_LISTENER_EVENT* event);
    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection, void* context,
                                                     QUIC_CONNECTION_EVENT* event);
    static QUIC_STATUS QUIC_API stream_callback(HQUIC stream, void* context,
                                                 QUIC_STREAM_EVENT* event);

    struct FileStreamContext;
    static QUIC_STATUS QUIC_API file_stream_callback(HQUIC stream, void* context,
                                                      QUIC_STREAM_EVENT* event);

    struct VideoInStreamContext;
    struct VideoOutStreamContext;
    static QUIC_STATUS QUIC_API video_in_stream_callback(HQUIC stream, void* context,
                                                          QUIC_STREAM_EVENT* event);
    static QUIC_STATUS QUIC_API video_out_stream_callback(HQUIC stream, void* context,
                                                           QUIC_STREAM_EVENT* event);

    QUIC_STATUS on_new_connection(HQUIC listener, QUIC_LISTENER_EVENT* event);
    QUIC_STATUS on_connection_event(HQUIC connection, uint32_t session_id,
                                    QUIC_CONNECTION_EVENT* event);
    QUIC_STATUS on_stream_event(HQUIC stream, uint32_t session_id,
                                QUIC_STREAM_EVENT* event);
    QUIC_STATUS on_unconnected_query(QUIC_LISTENER_EVENT* event);
    QUIC_STATUS on_file_stream_event(HQUIC stream, FileStreamContext* ctx,
                                      QUIC_STREAM_EVENT* event);

    bool send_control_on_stream(HQUIC stream, protocol::ControlMessageType type,
                                const uint8_t* payload, size_t payload_len);

    void process_stream_data(uint32_t session_id, const uint8_t* data, size_t len);

    void process_video_stream_data(uint32_t session_id, const uint8_t* data, size_t len);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::atomic<bool> running_{false};

    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
    uint32_t next_session_id_ = 1;

    std::atomic<uint64_t> reliable_send_failures_{0};

    std::string query_server_name_;
    uint16_t    query_max_users_ = 0;
    bool        query_password_locked_ = false;

    std::mutex buffers_mutex_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> recv_buffers_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> video_recv_buffers_;

    ThreadQueue<IncomingMessage> control_incoming_;
    ThreadQueue<DataPacket> data_incoming_;
    ThreadQueue<FileUploadEvent> file_uploads_;
    ThreadQueue<FileDownloadRequest> file_downloads_;
    ThreadQueue<SessionDisconnect> disconnects_;
};

}
