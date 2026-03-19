#include <server/quic_server.h>
#include <parties/quic_common.h>
#include <parties/protocol.h>
#include <parties/profiler.h>

#include <parties/log.h>
#include <cstring>

namespace parties::server {

// Context stored in QUIC connection/stream user data
struct ConnectionContext {
    QuicServer* server;
    uint32_t session_id;
};

// Context for file transfer streams (ephemeral, one per file upload/download)
struct QuicServer::FileStreamContext {
    QuicServer* server;
    uint32_t session_id;
    std::vector<uint8_t> buffer;   // accumulated received data
    bool header_parsed;
    uint8_t stream_type;           // STREAM_TYPE_FILE_UPLOAD or STREAM_TYPE_FILE_DOWNLOAD
    uint64_t attachment_id;
};

QuicServer::QuicServer() = default;

QuicServer::~QuicServer() {
    stop();
}

bool QuicServer::start(const std::string& listen_ip, uint16_t port, size_t max_clients,
                       const std::string& cert_file, const std::string& key_file) {
	ZoneScopedN("QuicServer::start");
    api_ = parties::quic_api();
    if (!api_) {
        LOG_ERROR("MsQuic not initialized");
        return false;
    }

    QUIC_STATUS status;

    // Registration
    QUIC_REGISTRATION_CONFIG reg_config = { "parties_server", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    status = api_->RegistrationOpen(&reg_config, &registration_);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("RegistrationOpen failed: {:#x}", (unsigned long)status);
        return false;
    }

    // Configuration with settings
    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = 60000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 10;  // Control + video + up to 8 file transfers
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    settings.IsSet.ServerResumptionLevel = TRUE;
    // Low-latency: disable send buffering (flush immediately) and pacing
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;
    settings.PacingEnabled = FALSE;
    settings.IsSet.PacingEnabled = TRUE;
    // MaxBytesPerKey left at default — MsQuic rejects values > QUIC_DEFAULT_MAX_BYTES_PER_KEY

    QUIC_BUFFER alpn = parties::make_alpn();
    status = api_->ConfigurationOpen(registration_, &alpn, 1, &settings,
                                      sizeof(settings), nullptr, &configuration_);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("ConfigurationOpen failed: {:#x}", (unsigned long)status);
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    // Load TLS credentials — QuicTLS uses PEM files directly
    {
        QUIC_CERTIFICATE_FILE cert_file_config = {};
        cert_file_config.CertificateFile = cert_file.c_str();
        cert_file_config.PrivateKeyFile = key_file.c_str();

        QUIC_CREDENTIAL_CONFIG cred_config = {};
        cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        cred_config.CertificateFile = &cert_file_config;

        status = api_->ConfigurationLoadCredential(configuration_, &cred_config);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConfigurationLoadCredential failed: {:#x}", (unsigned long)status);
            api_->ConfigurationClose(configuration_);
            configuration_ = nullptr;
            api_->RegistrationClose(registration_);
            registration_ = nullptr;
            return false;
        }
    }

    // Open and start listener
    status = api_->ListenerOpen(registration_, listener_callback, this, &listener_);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("ListenerOpen failed: {:#x}", (unsigned long)status);
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    QUIC_ADDR addr = {};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port);

    status = api_->ListenerStart(listener_, &alpn, 1, &addr);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("ListenerStart failed: {:#x}", (unsigned long)status);
        api_->ListenerClose(listener_);
        listener_ = nullptr;
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    running_ = true;
    LOG_INFO("Listening on port {}", port);
    return true;
}

void QuicServer::stop() {
	ZoneScopedN("QuicServer::stop");
    if (!running_) return;
    running_ = false;

    if (listener_) {
        api_->ListenerClose(listener_);
        listener_ = nullptr;
    }

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [id, session] : sessions_) {
            session->alive = false;
            if (session->quic_connection) {
                api_->ConnectionShutdown(session->quic_connection,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            }
        }
    }

    if (configuration_) {
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }
    if (registration_) {
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
    }

    LOG_INFO("QuicServer stopped");
}

// ── Control plane ──

bool QuicServer::send_to(uint32_t session_id, protocol::ControlMessageType type,
                          const uint8_t* payload, size_t payload_len) {
	ZoneScopedN("QuicServer::send_to");
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end() || !it->second->alive) return false;

    auto& session = it->second;
    if (!session->quic_control_stream) return false;

    return send_control_on_stream(session->quic_control_stream, type, payload, payload_len);
}

void QuicServer::broadcast(protocol::ControlMessageType type,
                            const uint8_t* payload, size_t payload_len) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        if (session->alive && session->authenticated && session->quic_control_stream) {
            send_control_on_stream(session->quic_control_stream, type, payload, payload_len);
        }
    }
}

void QuicServer::disconnect(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;

    it->second->alive = false;
    if (it->second->quic_connection) {
        api_->ConnectionShutdown(it->second->quic_connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}

std::shared_ptr<Session> QuicServer::get_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return nullptr;
    return it->second;
}

std::vector<std::shared_ptr<Session>> QuicServer::get_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(sessions_.size());
    for (auto& [id, session] : sessions_)
        result.push_back(session);
    return result;
}

// ── Data plane ──

bool QuicServer::send_datagram(uint32_t session_id, const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::send_datagram");
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end() || !it->second->alive || !it->second->quic_connection)
        return false;

    // Both the data buffer AND the QUIC_BUFFER descriptor must be heap-allocated
    // because DatagramSend is async — MsQuic stores only the pointer.
    auto* buf_data = new uint8_t[len];
    std::memcpy(buf_data, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf_data };
    QUIC_STATUS status = api_->DatagramSend(it->second->quic_connection,
        quic_buf, 1, QUIC_SEND_FLAG_NONE, quic_buf);

    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

void QuicServer::send_to_many(const std::vector<uint32_t>& session_ids,
                               const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::send_to_many");
    for (uint32_t id : session_ids)
        send_datagram(id, data, len);
}

bool QuicServer::send_stream(uint32_t session_id, const uint8_t* data, size_t len) {
    return send_video_to(session_id, data, len);
}

bool QuicServer::send_video_to(uint32_t session_id, const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::send_video_to");
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end() || !it->second->alive || !it->second->quic_video_stream)
        return false;

    // Wire format: [u32 len][data]
    size_t total_len = 4 + len;
    auto* buf_data = new uint8_t[total_len];
    uint32_t frame_len = static_cast<uint32_t>(len);
    std::memcpy(buf_data, &frame_len, 4);
    std::memcpy(buf_data + 4, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf_data };
    QUIC_STATUS status = api_->StreamSend(it->second->quic_video_stream, quic_buf, 1,
                                           QUIC_SEND_FLAG_NONE, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

void QuicServer::send_to_many_on_channel(const std::vector<uint32_t>& session_ids,
                                          uint8_t /*channel*/, const uint8_t* data, size_t len,
                                          uint32_t /*flags*/) {
	ZoneScopedN("QuicServer::send_to_many_on_channel");
    // Channel is ignored in QUIC (no ENet channels).
    // Reliability flags are ignored — datagrams are unreliable, streams are reliable.
    send_to_many(session_ids, data, len);
}

bool QuicServer::send_to_on_channel(uint32_t session_id, uint8_t /*channel*/,
                                     const uint8_t* data, size_t len, uint32_t /*flags*/) {
    return send_datagram(session_id, data, len);
}

// ── MsQuic callbacks ──

QUIC_STATUS QUIC_API QuicServer::listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event) {
    auto* server = static_cast<QuicServer*>(context);
    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        return server->on_new_connection(listener, event);
    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicServer::connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* ctx = static_cast<ConnectionContext*>(context);
    auto status = ctx->server->on_connection_event(connection, ctx->session_id, event);
    if (event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE)
        delete ctx;
    return status;
}

QUIC_STATUS QUIC_API QuicServer::stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<ConnectionContext*>(context);
    auto status = ctx->server->on_stream_event(stream, ctx->session_id, event);
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE)
        delete ctx;
    return status;
}

QUIC_STATUS QuicServer::on_new_connection(HQUIC /*listener*/, QUIC_LISTENER_EVENT* event) {
    HQUIC connection = event->NEW_CONNECTION.Connection;

    uint32_t session_id;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        session_id = next_session_id_++;
    }

    // Create connection context (leak-free: cleaned up in SHUTDOWN_COMPLETE)
    auto* ctx = new ConnectionContext{ this, session_id };
    api_->SetCallbackHandler(connection, (void*)connection_callback, ctx);

    QUIC_STATUS status = api_->ConnectionSetConfiguration(connection, configuration_);
    if (QUIC_FAILED(status)) {
        delete ctx;
        return status;
    }

    // Create session
    auto session = std::make_shared<Session>();
    session->id = session_id;
    session->quic_connection = connection;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = session;
    }

    LOG_INFO("New connection: session {}", session_id);
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicServer::on_connection_event(HQUIC connection, uint32_t session_id,
                                             QUIC_CONNECTION_EVENT* event) {
    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        LOG_INFO("Session {} connected", session_id);
        // Send resumption ticket for 0-RTT support
        api_->ConnectionSendResumptionTicket(connection,
            QUIC_SEND_RESUMPTION_FLAG_NONE, 0, nullptr);
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        // Client opened a bidirectional stream
        HQUIC stream = event->PEER_STREAM_STARTED.Stream;

        bool is_file_stream = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                if (!it->second->quic_control_stream) {
                    it->second->quic_control_stream = stream;
                    LOG_INFO("Session {} opened control stream", session_id);
                } else if (!it->second->quic_video_stream) {
                    it->second->quic_video_stream = stream;
                    LOG_INFO("Session {} opened video stream", session_id);
                } else {
                    is_file_stream = true;
                }
            }
        }

        if (is_file_stream) {
            auto* fctx = new FileStreamContext{ this, session_id, {}, false, 0, 0 };
            api_->SetCallbackHandler(stream, (void*)file_stream_callback, fctx);
            LOG_INFO("Session {} opened file transfer stream", session_id);
        } else {
            auto* ctx = new ConnectionContext{ this, session_id };
            api_->SetCallbackHandler(stream, (void*)stream_callback, ctx);
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        // Voice/video data received as datagram
        auto* buf = event->DATAGRAM_RECEIVED.Buffer;
        if (buf->Length > 0) {
            DataPacket pkt;
            pkt.session_id = session_id;
            pkt.packet_type = buf->Buffer[0];
            pkt.channel_id = 0;
            pkt.reliable = false;
            pkt.data.assign(buf->Buffer + 1, buf->Buffer + buf->Length);
            data_incoming_.push(std::move(pkt));
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
        // Free the QUIC_BUFFER + its data when send completes
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State)) {
            auto* quic_buf = static_cast<QUIC_BUFFER*>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            if (quic_buf) {
                delete[] quic_buf->Buffer;
                delete quic_buf;
            }
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_RESUMED:
        LOG_INFO("Session {} resumed (0-RTT)", session_id);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        LOG_INFO("Session {} shutting down", session_id);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        LOG_INFO("Session {} shutdown complete", session_id);

        // Notify disconnect callback
        if (on_disconnect)
            on_disconnect(session_id);

        // Remove session
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.erase(session_id);
        }
        {
            std::lock_guard<std::mutex> lock(buffers_mutex_);
            recv_buffers_.erase(session_id);
            video_recv_buffers_.erase(session_id);
        }

        // ConnectionContext is freed in connection_callback after this returns
        api_->ConnectionClose(connection);
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicServer::on_stream_event(HQUIC stream, uint32_t session_id,
                                         QUIC_STREAM_EVENT* event) {
    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Route to control or video stream processor
        bool is_video = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end())
                is_video = (stream == it->second->quic_video_stream);
        }
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& buf = event->RECEIVE.Buffers[i];
            if (is_video)
                process_video_stream_data(session_id, buf.Buffer, buf.Length);
            else
                process_stream_data(session_id, buf.Buffer, buf.Length);
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Peer finished sending on this stream
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        // Free the QUIC_BUFFER and its data that we allocated in send_control_on_stream
        auto* quic_buf = static_cast<QUIC_BUFFER*>(event->SEND_COMPLETE.ClientContext);
        if (quic_buf) {
            delete[] quic_buf->Buffer;
            delete quic_buf;
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        api_->StreamClose(stream);

        // Remove stream reference
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                if (it->second->quic_control_stream == stream)
                    it->second->quic_control_stream = nullptr;
                else if (it->second->quic_video_stream == stream)
                    it->second->quic_video_stream = nullptr;
            }
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ── Internal helpers ──

bool QuicServer::send_control_on_stream(HQUIC stream,
                                         protocol::ControlMessageType type,
                                         const uint8_t* payload, size_t payload_len) {
    // Wire format: [u32 length][u16 type][payload]
    uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
    size_t total_len = 6 + payload_len;

    auto* buf_data = new uint8_t[total_len];
    std::memcpy(buf_data, &msg_len, 4);
    uint16_t msg_type = static_cast<uint16_t>(type);
    std::memcpy(buf_data + 4, &msg_type, 2);
    if (payload_len > 0)
        std::memcpy(buf_data + 6, payload, payload_len);

    // MsQuic takes ownership of the QUIC_BUFFER struct but not the data.
    // We need to free the data in SEND_COMPLETE.
    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf_data };

    QUIC_STATUS status = api_->StreamSend(stream, quic_buf, 1,
                                           QUIC_SEND_FLAG_NONE, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

void QuicServer::process_stream_data(uint32_t session_id,
                                      const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::process_stream_data");
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    auto& buffer = recv_buffers_[session_id];
    buffer.insert(buffer.end(), data, data + len);

    // Parse complete messages: [u32 length][u16 type][payload]
    while (buffer.size() >= 6) {
        uint32_t msg_len;
        std::memcpy(&msg_len, buffer.data(), 4);

        if (msg_len < 2 || msg_len > 1024 * 1024) {
            LOG_ERROR("Invalid message length {} from session {}", msg_len, session_id);
            buffer.clear();
            break;
        }

        size_t total_needed = 4 + msg_len;
        if (buffer.size() < total_needed) break;  // Wait for more data

        uint16_t raw_type;
        std::memcpy(&raw_type, buffer.data() + 4, 2);

        uint32_t payload_len = msg_len - 2;
        IncomingMessage msg;
        msg.session_id = session_id;
        msg.type = static_cast<protocol::ControlMessageType>(raw_type);
        if (payload_len > 0)
            msg.payload.assign(buffer.data() + 6, buffer.data() + 6 + payload_len);

        control_incoming_.push(std::move(msg));

        buffer.erase(buffer.begin(), buffer.begin() + total_needed);
    }
}

void QuicServer::process_video_stream_data(uint32_t session_id,
                                            const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::process_video_stream_data");
    // Parse complete frames and collect them, then forward outside the lock
    // to avoid holding buffers_mutex_ during network sends.
    struct PendingFrame {
        uint8_t packet_type;
        std::vector<uint8_t> data;
    };
    std::vector<PendingFrame> frames;

    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        auto& buffer = video_recv_buffers_[session_id];
        buffer.insert(buffer.end(), data, data + len);

        while (buffer.size() >= 4) {
            uint32_t frame_len;
            std::memcpy(&frame_len, buffer.data(), 4);

            if (frame_len == 0 || frame_len > 4 * 1024 * 1024) {
                LOG_ERROR("Invalid video frame length {} from session {}", frame_len, session_id);
                buffer.clear();
                break;
            }

            size_t total_needed = 4 + frame_len;
            if (buffer.size() < total_needed) break;

            if (frame_len > 1) {
                PendingFrame f;
                f.packet_type = buffer[4];
                f.data.assign(buffer.data() + 5, buffer.data() + 4 + frame_len);
                frames.push_back(std::move(f));
            }

            buffer.erase(buffer.begin(), buffer.begin() + total_needed);
        }
    }

    // Forward outside the lock
    for (auto& f : frames) {
        if (on_video_frame) {
            on_video_frame(session_id, f.packet_type, f.data.data(), f.data.size());
        } else {
            DataPacket pkt;
            pkt.session_id = session_id;
            pkt.packet_type = f.packet_type;
            pkt.channel_id = 0;
            pkt.reliable = true;
            pkt.data = std::move(f.data);
            data_incoming_.push(std::move(pkt));
        }
    }
}

// ── File transfer streams ──

QUIC_STATUS QUIC_API QuicServer::file_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<FileStreamContext*>(context);
    auto status = ctx->server->on_file_stream_event(stream, ctx, event);
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        ctx->server->api_->StreamClose(stream);
        delete ctx;
    }
    return status;
}

QUIC_STATUS QuicServer::on_file_stream_event(HQUIC stream, FileStreamContext* ctx,
                                              QUIC_STREAM_EVENT* event) {
    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& buf = event->RECEIVE.Buffers[i];
            ctx->buffer.insert(ctx->buffer.end(), buf.Buffer, buf.Buffer + buf.Length);
        }

        // Parse header: [type(1)][attachment_id(8)] = 9 bytes
        if (!ctx->header_parsed && ctx->buffer.size() >= 9) {
            ctx->stream_type = ctx->buffer[0];
            std::memcpy(&ctx->attachment_id, ctx->buffer.data() + 1, 8);
            ctx->header_parsed = true;
            ctx->buffer.erase(ctx->buffer.begin(), ctx->buffer.begin() + 9);
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        if (!ctx->header_parsed) break;

        if (ctx->stream_type == protocol::STREAM_TYPE_FILE_UPLOAD) {
            // Upload complete — push event for server main loop
            FileUploadEvent ev;
            ev.session_id = ctx->session_id;
            ev.attachment_id = ctx->attachment_id;
            ev.data = std::move(ctx->buffer);
            file_uploads_.push(std::move(ev));
        } else if (ctx->stream_type == protocol::STREAM_TYPE_FILE_DOWNLOAD) {
            // Download request — push for server main loop to handle
            FileDownloadRequest req;
            req.session_id = ctx->session_id;
            req.attachment_id = ctx->attachment_id;
            req.stream = stream;
            file_downloads_.push(std::move(req));
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* qb = static_cast<QUIC_BUFFER*>(event->SEND_COMPLETE.ClientContext);
        if (qb) {
            delete[] qb->Buffer;
            delete qb;
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Cleanup handled in file_stream_callback after this returns
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

bool QuicServer::send_file_on_stream(HQUIC stream, const uint8_t* data, size_t len) {
    auto* buf_data = new uint8_t[len];
    std::memcpy(buf_data, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf_data };
    QUIC_STATUS status = api_->StreamSend(stream, quic_buf, 1,
                                           QUIC_SEND_FLAG_FIN, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

} // namespace parties::server
