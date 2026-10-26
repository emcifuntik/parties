#pragma once

#include <server/session.h>
#include <server/video_ingress_queue.h>
#include <parties/thread_queue.h>
#include <parties/protocol.h>
#include <parties/quic_common.h>
#include <parties/net_common.h>

#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
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
    std::shared_ptr<Session> session;
};

class SessionRegistry {
public:
    uint32_t allocate_id() {
        std::lock_guard lock(m_);
        return next_id_++;
    }

    void insert(std::shared_ptr<Session> session) {
        std::lock_guard lock(m_);
        const uint32_t id = session->id;
        sessions_[id] = std::move(session);
    }

    std::shared_ptr<Session> find(uint32_t id) const {
        std::lock_guard lock(m_);
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<Session>> snapshot() const {
        std::lock_guard lock(m_);
        std::vector<std::shared_ptr<Session>> result;
        result.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_) result.push_back(session);
        return result;
    }

    std::shared_ptr<Session> remove(uint32_t id) {
        std::lock_guard lock(m_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return nullptr;
        auto session = std::move(it->second);
        sessions_.erase(it);
        return session;
    }

    uint16_t count_authenticated() const {
        std::lock_guard lock(m_);
        uint16_t count = 0;
        for (const auto& [id, session] : sessions_)
            if (session->phase.authenticated()) ++count;
        return count;
    }

private:
    mutable std::mutex m_;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
    uint32_t next_id_ = 1;
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

    void disconnect(uint32_t session_id);

    std::shared_ptr<Session> get_session(uint32_t session_id);

    std::vector<std::shared_ptr<Session>> get_sessions();


    void set_server_info(std::string name, uint16_t max_users, bool password_locked);

    ThreadQueue<SessionDisconnect>& disconnects() { return disconnects_; }

    VideoIngressQueue& video_ingress() { return video_ingress_; }

    ThreadQueue<DataPacket>& data_incoming() { return data_incoming_; }

    bool send_datagram(uint32_t session_id, const uint8_t* data, size_t len,
                       DatagramPriority priority = DatagramPriority::Normal);

    void send_to_many(const std::vector<uint32_t>& session_ids,
                      const uint8_t* data, size_t len,
                      DatagramPriority priority = DatagramPriority::Normal);

    bool send_video_frame_to(const std::shared_ptr<Session>& session,
                             std::shared_ptr<const std::vector<uint8_t>> frame);

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

    struct SendCtx;
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

    bool send_control(Session& session, protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    void process_stream_data(uint32_t session_id, const uint8_t* data, size_t len);

    void process_video_stream_data(const std::shared_ptr<Session>& session,
                                   const uint8_t* data, size_t len);

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    std::atomic<bool> running_{false};

    SessionRegistry registry_;

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
    VideoIngressQueue video_ingress_;
};

}
