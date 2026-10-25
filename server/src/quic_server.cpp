#include <server/quic_server.h>
#include "session_transport.h"
#include <parties/quic_common.h>
#include <parties/protocol.h>
#include <parties/server_query.h>
#include <parties/profiler.h>

#include <parties/log.h>
#include <parties/video_common.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace parties::server {

static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Session::Session(uint32_t session_id) : id(session_id) {}
Session::~Session() = default;

struct ConnectionContext {
    QuicServer* server;
    uint32_t session_id;
};

struct QuicServer::SendCtx {
    QUIC_BUFFER bufs[2]{};
    uint32_t    buf_count = 0;
    std::vector<uint8_t> owned;
    std::shared_ptr<const std::vector<uint8_t>> shared;
    std::shared_ptr<Session> session;
    uint32_t    len = 0;

    void uncharge() {
        if (session && len) {
            session->transport_stats.video_outstanding_bytes_.fetch_sub(len, std::memory_order_relaxed);
            len = 0;
        }
    }
};

class LogThrottle {
public:
    std::optional<uint32_t> allow() {
        std::lock_guard lock(m_);
        const int64_t now = now_us();
        if (last_us_ && now - *last_us_ < 1'000'000) {
            ++suppressed_;
            return std::nullopt;
        }
        last_us_ = now;
        return std::exchange(suppressed_, 0);
    }

private:
    std::mutex             m_;
    std::optional<int64_t> last_us_;
    uint32_t               suppressed_ = 0;
};

static LogThrottle video_reject_log_throttle;
static LogThrottle video_ingress_log_throttle;

struct RejectedStreamCtx {
    const QUIC_API_TABLE* api = nullptr;
};

static QUIC_STATUS QUIC_API rejected_stream_callback(HQUIC stream, void* context,
                                                     QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<RejectedStreamCtx*>(context);
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        ctx->api->StreamClose(stream);
        delete ctx;
    }
    return QUIC_STATUS_SUCCESS;
}

struct QuicServer::VideoInStreamContext {
    QuicServer* server = nullptr;
    uint32_t    session_id = 0;
    std::shared_ptr<Session> session;
    std::vector<uint8_t> buf;
    int64_t     charged = 0;
    bool        dead = false;

    void uncharge() {
        if (charged && session)
            session->transport_->uncharge_ingress(charged);
        charged = 0;
    }

    void abort(HQUIC stream) {
        if (dead) return;
        dead = true;
        std::vector<uint8_t>().swap(buf);
        uncharge();
        server->api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
};

struct QuicServer::VideoOutStreamContext {
    QuicServer* server = nullptr;
    std::shared_ptr<Session> session;
    std::shared_ptr<const std::vector<uint8_t>> frame;
    uint8_t     type_byte = protocol::STREAM_TYPE_VIDEO_FRAME;
    QUIC_BUFFER bufs[2]{};
    std::atomic<uint32_t> charged{0};

    void uncharge() {
        uint32_t n = charged.exchange(0, std::memory_order_acq_rel);
        if (n && session)
            session->transport_stats.video_outstanding_bytes_.fetch_sub(n, std::memory_order_relaxed);
    }
};

struct QuicServer::FileStreamContext {
    QuicServer* server;
    uint32_t session_id;
    std::vector<uint8_t> buffer;
    bool header_parsed;
    uint8_t stream_type;
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

    QUIC_REGISTRATION_CONFIG reg_config = { "parties_server", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    status = api_->RegistrationOpen(&reg_config, &registration_);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("RegistrationOpen failed: {:#x}", (unsigned long)status);
        return false;
    }

    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = 60000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 10;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    // Per-frame video streams (protocol 1.2): one unidirectional stream per
    // encoded frame. 128 in flight covers a few seconds of frames at 60 fps
    // under loss; the default 64 KB receive window would window-limit
    // keyframes, so raise it to the largest frame we accept.
    settings.PeerUnidiStreamCount = 128;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.StreamRecvWindowUnidiDefault = 4 * 1024 * 1024;
    settings.IsSet.StreamRecvWindowUnidiDefault = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    // Session resumption (fast reconnect via ticket) but NOT 0-RTT: TLS 1.3
    // early data is replayable, and our only early message (AUTH_IDENTITY) must
    // not be replayable. RESUME_ONLY keeps the latency win of resumption without
    // accepting replayable early data.
    settings.ServerResumptionLevel = QUIC_SERVER_RESUME_ONLY;
    settings.IsSet.ServerResumptionLevel = TRUE;
    // Low-latency: no send buffering (data is handed to the wire immediately
    // and SEND_COMPLETE fires on acknowledgement, which is what the per-viewer
    // video backlog accounting relies on). Pacing stays ON: bursting a whole
    // keyframe into the network is exactly what causes loss and head-of-line
    // stalls on constrained links.
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;
    settings.PacingEnabled = TRUE;
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

    for (const auto& session : registry_.snapshot())
        session->transport_->shutdown_connection(*api_);

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

bool QuicServer::send_to(uint32_t session_id, protocol::ControlMessageType type,
                          const uint8_t* payload, size_t payload_len) {
	ZoneScopedN("QuicServer::send_to");
    auto session = registry_.find(session_id);
    return session && send_control(*session, type, payload, payload_len);
}

void QuicServer::disconnect(uint32_t session_id) {
    if (auto session = registry_.find(session_id))
        session->transport_->shutdown_connection(*api_);
}

std::shared_ptr<Session> QuicServer::get_session(uint32_t session_id) {
    return registry_.find(session_id);
}

std::vector<std::shared_ptr<Session>> QuicServer::get_sessions() {
    return registry_.snapshot();
}

bool QuicServer::send_datagram(uint32_t session_id, const uint8_t* data, size_t len,
                               DatagramPriority priority) {
	ZoneScopedN("QuicServer::send_datagram");
    std::shared_ptr<Session> session = get_session(session_id);
    if (!session) return false;

    // Both the data buffer AND the QUIC_BUFFER descriptor must be heap-allocated
    // because DatagramSend is async — MsQuic stores only the pointer.
    auto* buf_data = new uint8_t[len];
    std::memcpy(buf_data, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf_data };
    const auto status = session->transport_->send_datagram(*api_, quic_buf,
        priority == DatagramPriority::AheadOfVideo ? QUIC_SEND_FLAG_DGRAM_PRIORITY
                                                   : QUIC_SEND_FLAG_NONE,
        quic_buf);

    if (!status || QUIC_FAILED(*status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

void QuicServer::send_to_many(const std::vector<uint32_t>& session_ids,
                               const uint8_t* data, size_t len,
                              DatagramPriority priority) {
	ZoneScopedN("QuicServer::send_to_many");
    for (uint32_t id : session_ids)
        send_datagram(id, data, len, priority);
}

bool QuicServer::send_video_frame_to(const std::shared_ptr<Session>& session,
                                     std::shared_ptr<const std::vector<uint8_t>> frame) {
	ZoneScopedN("QuicServer::send_video_frame_to");
    if (!session || !frame) return false;
    if (1 + frame->size() > VIDEO_FRAME_MAX_BYTES) return false;

    if (session->supports_frame_streams()) {
        auto* ctx = new VideoOutStreamContext;
        ctx->server  = this;
        ctx->session = session;
        ctx->frame   = std::move(frame);
        ctx->bufs[0] = QUIC_BUFFER{ 1, &ctx->type_byte };
        ctx->bufs[1] = QUIC_BUFFER{ static_cast<uint32_t>(ctx->frame->size()),
                                    const_cast<uint8_t*>(ctx->frame->data()) };

        switch (session->transport_->send_on_new_frame_stream(*api_, video_out_stream_callback, ctx,
                                                               ctx->bufs, 2, ctx->charged)) {
        case SessionTransport::FrameStreamResult::Sent:
            return true;
        case SessionTransport::FrameStreamResult::NotConnected:
            delete ctx;
            return false;
        case SessionTransport::FrameStreamResult::OpenFailed:
            delete ctx;
            reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        case SessionTransport::FrameStreamResult::SendFailed:
            reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return false;
    }

    auto* ctx = new SendCtx;
    ctx->session = session;
    ctx->shared = std::move(frame);
    ctx->owned.resize(5);
    uint32_t frame_len = static_cast<uint32_t>(1 + ctx->shared->size());
    std::memcpy(ctx->owned.data(), &frame_len, 4);
    ctx->owned[4] = protocol::VIDEO_FRAME_PACKET_TYPE;
    ctx->bufs[0] = QUIC_BUFFER{ 5, ctx->owned.data() };
    ctx->bufs[1] = QUIC_BUFFER{ static_cast<uint32_t>(ctx->shared->size()),
                                const_cast<uint8_t*>(ctx->shared->data()) };
    ctx->buf_count = 2;
    ctx->len = 5 + static_cast<uint32_t>(ctx->shared->size());

    session->transport_stats.video_outstanding_bytes_.fetch_add(ctx->len, std::memory_order_relaxed);
    const auto status = session->transport_->send_on(*api_, SessionTransport::Stream::LegacyVideo,
                                                     ctx->bufs, ctx->buf_count, ctx);
    if (!status || QUIC_FAILED(*status)) {
        ctx->uncharge();
        delete ctx;
        if (status) reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

QUIC_STATUS QUIC_API QuicServer::listener_callback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event) {
    auto* server = static_cast<QuicServer*>(context);
    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        return server->on_new_connection(listener, event);
    case QUIC_LISTENER_EVENT_UNCONNECTED_QUERY:
        return server->on_unconnected_query(event);
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

void QuicServer::set_server_info(std::string name, uint16_t max_users, bool password_locked) {
    query_server_name_ = std::move(name);
    query_max_users_ = max_users;
    query_password_locked_ = password_locked;
}

QUIC_STATUS QuicServer::on_unconnected_query(QUIC_LISTENER_EVENT* event) {
    ZoneScopedN("QuicServer::on_unconnected_query");
    auto& q = event->UNCONNECTED_QUERY;

    if (!parties::is_server_query_request(q.Payload, q.PayloadLength)) {
        q.ReplyBufferLength = 0;
        return QUIC_STATUS_SUCCESS;
    }

    parties::ServerQueryInfo info;
    info.protocol_version = protocol::PROTOCOL_VERSION;
    info.server_name      = query_server_name_;
    info.max_users        = query_max_users_;
    info.password_locked  = query_password_locked_;

    info.current_users = registry_.count_authenticated();

    uint32_t token = parties::server_query_request_token(q.Payload, q.PayloadLength);
    std::vector<uint8_t> reply = parties::build_server_query_reply(token, info);

    if (reply.size() > q.ReplyBufferLength) {
        q.ReplyBufferLength = 0;
        return QUIC_STATUS_SUCCESS;
    }

    std::memcpy(q.ReplyBuffer, reply.data(), reply.size());
    q.ReplyBufferLength = static_cast<uint16_t>(reply.size());
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicServer::on_new_connection(HQUIC, QUIC_LISTENER_EVENT* event) {
    HQUIC connection = event->NEW_CONNECTION.Connection;

    const uint32_t session_id = registry_.allocate_id();

    auto* ctx = new ConnectionContext{ this, session_id };
    api_->SetCallbackHandler(connection, (void*)connection_callback, ctx);

    QUIC_STATUS status = api_->ConnectionSetConfiguration(connection, configuration_);
    if (QUIC_FAILED(status)) {
        delete ctx;
        return status;
    }

    auto session = std::make_shared<Session>(session_id);
    session->transport_ = std::make_unique<SessionTransport>(connection, session->transport_stats);
    registry_.insert(std::move(session));

    LOG_INFO("New connection: session {}", session_id);
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicServer::on_connection_event(HQUIC connection, uint32_t session_id,
                                             QUIC_CONNECTION_EVENT* event) {
    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        LOG_INFO("Session {} connected", session_id);
        api_->ConnectionSendResumptionTicket(connection,
            QUIC_SEND_RESUMPTION_FLAG_NONE, 0, nullptr);
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = event->PEER_STREAM_STARTED.Stream;

        if (event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) {
            std::shared_ptr<Session> session = registry_.find(session_id);
            if (!session || !session->phase.admits_video_streams()) {
                if (const auto suppressed = video_reject_log_throttle.allow()) {
                    LOG_DEBUG("Session {}: refused per-frame video stream (not an active sharer); {} more refusal(s) since the last line",
                              session_id, *suppressed);
                }
                auto* rctx = new RejectedStreamCtx{ api_ };
                api_->SetCallbackHandler(stream, (void*)rejected_stream_callback, rctx);
                api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                break;
            }

            auto* vctx = new VideoInStreamContext;
            vctx->server     = this;
            vctx->session_id = session_id;
            vctx->session    = std::move(session);
            api_->SetCallbackHandler(stream, (void*)video_in_stream_callback, vctx);
            break;
        }

        bool is_file_stream = false;
        if (auto session = registry_.find(session_id)) {
            switch (session->transport_->adopt_peer_bidi_stream(stream)) {
            case SessionTransport::PeerStreamRole::Control:
                LOG_INFO("Session {} opened control stream", session_id);
                break;
            case SessionTransport::PeerStreamRole::LegacyVideo:
                LOG_INFO("Session {} opened video stream", session_id);
                break;
            case SessionTransport::PeerStreamRole::FileTransfer:
                is_file_stream = true;
                break;
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

        if (auto session = registry_.remove(session_id)) {
            session->transport_->retire_handles();
            disconnects_.push(SessionDisconnect{ std::move(session) });
        }
        {
            std::lock_guard<std::mutex> lock(buffers_mutex_);
            recv_buffers_.erase(session_id);
            video_recv_buffers_.erase(session_id);
        }

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
        std::shared_ptr<Session> session = get_session(session_id);
        bool is_video = false;
        if (session) {
            session->transport_->sample_min_rtt_if_due(*api_, stream, now_us());
            is_video = session->transport_->is_legacy_video_stream(stream);
        }
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& buf = event->RECEIVE.Buffers[i];
            if (is_video)
                process_video_stream_data(session, buf.Buffer, buf.Length);
            else
                process_stream_data(session_id, buf.Buffer, buf.Length);
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* ctx = static_cast<SendCtx*>(event->SEND_COMPLETE.ClientContext);
        if (ctx) {
            if (ctx->session) ctx->session->transport_->sample_min_rtt_if_due(*api_, stream, now_us());
            ctx->uncharge();
            delete ctx;
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (auto session = get_session(session_id))
            session->transport_->forget_stream(stream);
        api_->StreamClose(stream);
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

bool QuicServer::send_control(Session& session, protocol::ControlMessageType type,
                              const uint8_t* payload, size_t payload_len) {
    uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
    size_t total_len = 6 + payload_len;

    auto* ctx = new SendCtx;
    ctx->owned.resize(total_len);
    std::memcpy(ctx->owned.data(), &msg_len, 4);
    uint16_t msg_type = static_cast<uint16_t>(type);
    std::memcpy(ctx->owned.data() + 4, &msg_type, 2);
    if (payload_len > 0)
        std::memcpy(ctx->owned.data() + 6, payload, payload_len);
    ctx->bufs[0] = QUIC_BUFFER{ static_cast<uint32_t>(total_len), ctx->owned.data() };
    ctx->buf_count = 1;

    const auto status = session.transport_->send_on(*api_, SessionTransport::Stream::Control,
                                                    ctx->bufs, ctx->buf_count, ctx);
    if (!status || QUIC_FAILED(*status)) {
        delete ctx;
        if (status) reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
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

    while (buffer.size() >= 6) {
        uint32_t msg_len;
        std::memcpy(&msg_len, buffer.data(), 4);

        if (msg_len < 2 || msg_len > 1024 * 1024) {
            LOG_ERROR("Invalid message length {} from session {}", msg_len, session_id);
            buffer.clear();
            break;
        }

        size_t total_needed = 4 + msg_len;
        if (buffer.size() < total_needed) break;

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

void QuicServer::process_video_stream_data(const std::shared_ptr<Session>& session,
                                            const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::process_video_stream_data");
    struct PendingFrame {
        uint8_t packet_type;
        std::vector<uint8_t> data;
    };
    std::vector<PendingFrame> frames;

    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        auto& buffer = video_recv_buffers_[session->id];
        buffer.insert(buffer.end(), data, data + len);

        while (buffer.size() >= 4) {
            uint32_t frame_len;
            std::memcpy(&frame_len, buffer.data(), 4);

            if (frame_len == 0 || frame_len > 1 + VIDEO_FRAME_MAX_PAYLOAD_BYTES) {
                LOG_ERROR("Invalid video frame length {} from session {}", frame_len, session->id);
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

    for (auto& f : frames) {
        if (f.packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            video_ingress_.offer(session, f.data);
        } else {
            DataPacket pkt;
            pkt.session_id = session->id;
            pkt.packet_type = f.packet_type;
            pkt.channel_id = 0;
            pkt.reliable = true;
            pkt.data = std::move(f.data);
            data_incoming_.push(std::move(pkt));
        }
    }
}

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
            FileUploadEvent ev;
            ev.session_id = ctx->session_id;
            ev.attachment_id = ctx->attachment_id;
            ev.data = std::move(ctx->buffer);
            file_uploads_.push(std::move(ev));
        } else if (ctx->stream_type == protocol::STREAM_TYPE_FILE_DOWNLOAD) {
            FileDownloadRequest req;
            req.session_id = ctx->session_id;
            req.attachment_id = ctx->attachment_id;
            req.stream = stream;
            file_downloads_.push(std::move(req));
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<SendCtx*>(event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

bool QuicServer::send_file_on_stream(HQUIC stream, const uint8_t* data, size_t len) {
    auto* ctx = new SendCtx;
    ctx->owned.assign(data, data + len);
    ctx->bufs[0] = QUIC_BUFFER{ static_cast<uint32_t>(len), ctx->owned.data() };
    ctx->buf_count = 1;

    QUIC_STATUS status = api_->StreamSend(stream, ctx->bufs, ctx->buf_count,
                                           QUIC_SEND_FLAG_FIN, ctx);
    if (QUIC_FAILED(status)) {
        delete ctx;
        return false;
    }
    return true;
}

QUIC_STATUS QUIC_API QuicServer::video_in_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<VideoInStreamContext*>(context);
    QuicServer* self = ctx->server;
    const QUIC_API_TABLE* api = self->api_;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (ctx->dead) break;
        ctx->session->transport_->sample_min_rtt_if_due(*api, stream, now_us());

        int64_t incoming = 0;
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++)
            incoming += event->RECEIVE.Buffers[i].Length;
        ctx->charged += incoming;
        const int64_t buffered = ctx->session->transport_->charge_ingress(incoming);
        if (buffered > Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES) {
            if (const auto suppressed = video_ingress_log_throttle.allow()) {
                LOG_WARN("Session {}: per-frame video ingress over the {} byte buffer cap ({} buffered) - aborting stream; {} more since the last line",
                         ctx->session_id, Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES, buffered, *suppressed);
            }
            ctx->abort(stream);
            break;
        }

        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& b = event->RECEIVE.Buffers[i];
            ctx->buf.insert(ctx->buf.end(), b.Buffer, b.Buffer + b.Length);
        }

        if ((!ctx->buf.empty() && ctx->buf[0] != protocol::STREAM_TYPE_VIDEO_FRAME) ||
            ctx->buf.size() > 1 + VIDEO_FRAME_MAX_PAYLOAD_BYTES) {
            if (const auto suppressed = video_ingress_log_throttle.allow()) {
                LOG_WARN("Session {}: invalid per-frame video stream (first byte {:#x}, {} bytes) - aborting; {} more since the last line",
                         ctx->session_id, ctx->buf.empty() ? 0u : (unsigned)ctx->buf[0], ctx->buf.size(), *suppressed);
            }
            ctx->abort(stream);
            break;
        }

        if (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) {
            if (ctx->buf.size() >= 1 + VIDEO_FRAME_HEADER_SIZE) {
                self->video_ingress_.offer(ctx->session, std::span<const uint8_t>(ctx->buf).subspan(1));
            } else {
                LOG_DEBUG("Session {}: dropping truncated per-frame video stream ({} bytes)",
                          ctx->session_id, ctx->buf.size());
            }
            std::vector<uint8_t>().swap(ctx->buf);
            ctx->uncharge();
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // FIN already handled through the RECEIVE flag; MsQuic completes the
        // shutdown of a receive-only stream on its own.
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        ctx->abort(stream);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        ctx->uncharge();
        api->StreamClose(stream);
        delete ctx;
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicServer::video_out_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<VideoOutStreamContext*>(context);
    const QUIC_API_TABLE* api = ctx->server->api_;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event->START_COMPLETE.Status)) {
            LOG_DEBUG("Per-frame video stream start failed: {:#x}",
                      (unsigned long)event->START_COMPLETE.Status);
            ctx->server->reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        ctx->session->transport_->sample_min_rtt_if_due(*api, stream, now_us());
        ctx->uncharge();
        break;

    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        ctx->uncharge();
        api->StreamClose(stream);
        delete ctx;
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

}
