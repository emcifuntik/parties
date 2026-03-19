// MsQuic-based QUIC transport for Windows / Linux.
//
// Defines NetClient::Impl and all NetClient method bodies.
// On Apple platforms, net_client_apple.mm is compiled instead.

#include <client/net_client.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>
#include <parties/protocol.h>
#include <parties/crypto.h>

#include "net_client_parsing.h"

#include <parties/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace parties::client {

// ── MsQuic Impl ──────────────────────────────────────────────────────────────

struct NetClient::Impl {
    NetClient& parent;

    const QUIC_API_TABLE* api  = nullptr;
    HQUIC registration         = nullptr;
    HQUIC configuration        = nullptr;
    HQUIC connection           = nullptr;
    HQUIC control_stream       = nullptr;
    HQUIC video_stream         = nullptr;

    std::atomic<bool> connected{false};
    std::atomic<bool> connecting{false};
    std::atomic<bool> connect_failed_{false};
    std::mutex        write_mutex;

    std::mutex           buffer_mutex;
    std::vector<uint8_t> recv_buffer;
    std::mutex           video_buffer_mutex;
    std::vector<uint8_t> video_recv_buffer;

    std::string server_fingerprint;

    explicit Impl(NetClient& p) : parent(p) {}

    // ── connect / disconnect ──────────────────────────────────────────────

    bool connect(const std::string& host, uint16_t port,
                 const uint8_t* ticket, size_t ticket_len)
    {
        ZoneScopedN("NetClient::connect");
        if (connected) return false;

        api = parties::quic_api();
        if (!api) {
            LOG_ERROR("MsQuic not initialized");
            return false;
        }

        QUIC_STATUS status;

        QUIC_REGISTRATION_CONFIG reg_cfg = { "parties_client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
        status = api->RegistrationOpen(&reg_cfg, &registration);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("RegistrationOpen: {:#x}", (unsigned long)status);
            return false;
        }

        QUIC_SETTINGS settings = {};
        settings.IdleTimeoutMs          = 60000; settings.IsSet.IdleTimeoutMs          = TRUE;
        settings.KeepAliveIntervalMs    = 15000; settings.IsSet.KeepAliveIntervalMs    = TRUE;
        settings.DatagramReceiveEnabled = TRUE;  settings.IsSet.DatagramReceiveEnabled = TRUE;
        settings.SendBufferingEnabled   = FALSE; settings.IsSet.SendBufferingEnabled   = TRUE;
        settings.PacingEnabled          = FALSE; settings.IsSet.PacingEnabled          = TRUE;

        QUIC_BUFFER alpn = parties::make_alpn();
        status = api->ConfigurationOpen(registration, &alpn, 1, &settings,
                                        sizeof(settings), nullptr, &configuration);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConfigurationOpen: {:#x}", (unsigned long)status);
            cleanup_handles();
            return false;
        }

        QUIC_CREDENTIAL_CONFIG cred = {};
        cred.Type  = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT
                   | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION
                   | QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED
                   | QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;

        status = api->ConfigurationLoadCredential(configuration, &cred);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConfigurationLoadCredential: {:#x}", (unsigned long)status);
            cleanup_handles();
            return false;
        }

        status = api->ConnectionOpen(registration, s_connection_cb, this, &connection);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConnectionOpen: {:#x}", (unsigned long)status);
            cleanup_handles();
            return false;
        }

        if (ticket && ticket_len > 0) {
            status = api->SetParam(connection, QUIC_PARAM_CONN_RESUMPTION_TICKET,
                                   static_cast<uint32_t>(ticket_len), ticket);
            if (QUIC_FAILED(status))
                LOG_WARN("SetParam(RESUMPTION_TICKET): {:#x} (non-fatal)", (unsigned long)status);
        }

        status = api->ConnectionStart(connection, configuration,
                                      QUIC_ADDRESS_FAMILY_UNSPEC,
                                      host.c_str(), port);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConnectionStart: {:#x}", (unsigned long)status);
            cleanup_handles();
            return false;
        }

        LOG_INFO("Connecting to {}:{} (MsQuic)...", host, port);
        connecting      = true;
        connect_failed_ = false;
        return true;
    }

    void disconnect()
    {
        ZoneScopedN("NetClient::disconnect");
        if (!connected && !connecting && !connection) return;

        connected       = false;
        connecting      = false;
        connect_failed_ = false;

        if (connection) {
            api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        control_stream = nullptr;
        video_stream   = nullptr;
        cleanup_handles();

        { std::lock_guard lock(buffer_mutex);       recv_buffer.clear(); }
        { std::lock_guard lock(video_buffer_mutex); video_recv_buffer.clear(); }
        server_fingerprint.clear();
    }

    // ── send ─────────────────────────────────────────────────────────────

    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len)
    {
        ZoneScopedN("NetClient::send_message");
        std::lock_guard lock(write_mutex);
        if (!connected || !control_stream) return false;

        uint32_t msg_len   = static_cast<uint32_t>(2 + payload_len);
        size_t   total_len = 6 + payload_len;
        auto*    buf       = new uint8_t[total_len];

        std::memcpy(buf, &msg_len, 4);
        uint16_t t = static_cast<uint16_t>(type);
        std::memcpy(buf + 4, &t, 2);
        if (payload_len > 0) std::memcpy(buf + 6, payload, payload_len);

        auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf };
        if (QUIC_FAILED(api->StreamSend(control_stream, qb, 1, QUIC_SEND_FLAG_NONE, qb))) {
            delete[] buf; delete qb; return false;
        }
        return true;
    }

    bool send_data(const uint8_t* data, size_t len, bool /*reliable*/)
    {
        ZoneScopedN("NetClient::send_data");
        if (!connected || !connection) return false;

        auto* buf = new uint8_t[len];
        std::memcpy(buf, data, len);
        auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf };
        if (QUIC_FAILED(api->DatagramSend(connection, qb, 1, QUIC_SEND_FLAG_NONE, qb))) {
            delete[] buf; delete qb; return false;
        }
        return true;
    }

    bool send_video(const uint8_t* data, size_t len, bool /*reliable*/)
    {
        ZoneScopedN("NetClient::send_video");
        if (!connected || !video_stream) return false;

        size_t total_len = 4 + len;
        auto*  buf       = new uint8_t[total_len];
        uint32_t flen = static_cast<uint32_t>(len);
        std::memcpy(buf, &flen, 4);
        std::memcpy(buf + 4, data, len);

        auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf };
        if (QUIC_FAILED(api->StreamSend(video_stream, qb, 1, QUIC_SEND_FLAG_NONE, qb))) {
            delete[] buf; delete qb; return false;
        }
        return true;
    }

    // ── MsQuic callbacks ─────────────────────────────────────────────────

    static QUIC_STATUS QUIC_API s_connection_cb(HQUIC conn, void* ctx,
                                                 QUIC_CONNECTION_EVENT* ev)
    {
        return static_cast<Impl*>(ctx)->on_connection_event(conn, ev);
    }

    static QUIC_STATUS QUIC_API s_stream_cb(HQUIC stream, void* ctx,
                                             QUIC_STREAM_EVENT* ev)
    {
        return static_cast<Impl*>(ctx)->on_stream_event(stream, ev);
    }

    QUIC_STATUS on_connection_event(HQUIC conn, QUIC_CONNECTION_EVENT* ev)
    {
        switch (ev->Type) {

        case QUIC_CONNECTION_EVENT_CONNECTED: {
            LOG_INFO("QUIC connected");
            connecting = false;

            // Control stream
            HQUIC stream = nullptr;
            if (QUIC_SUCCEEDED(api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                               s_stream_cb, this, &stream)) &&
                QUIC_SUCCEEDED(api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
                control_stream = stream;
            } else {
                LOG_ERROR("Control stream failed");
                if (stream) api->StreamClose(stream);
            }

            // Video stream
            if (control_stream) {
                HQUIC vs = nullptr;
                if (QUIC_SUCCEEDED(api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                                   s_stream_cb, this, &vs)) &&
                    QUIC_SUCCEEDED(api->StreamStart(vs, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
                    video_stream = vs;
                } else {
                    LOG_ERROR("Video stream failed");
                    if (vs) api->StreamClose(vs);
                }
                connected = true;
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            auto* buf = ev->DATAGRAM_RECEIVED.Buffer;
            if (buf->Length > 0 && parent.on_data_received)
                parent.on_data_received(buf->Buffer, buf->Length);
            break;
        }

        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(ev->DATAGRAM_SEND_STATE_CHANGED.State)) {
                auto* qb = static_cast<QUIC_BUFFER*>(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
                if (qb) { delete[] qb->Buffer; delete qb; }
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
            auto* cert = static_cast<QUIC_BUFFER*>(ev->PEER_CERTIFICATE_RECEIVED.Certificate);
            if (cert && cert->Buffer && cert->Length > 0) {
                server_fingerprint = parties::sha256_hex(cert->Buffer, cert->Length);
                LOG_INFO("Server fingerprint: {}", server_fingerprint);
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED: {
            auto& t = ev->RESUMPTION_TICKET_RECEIVED;
            if (parent.on_resumption_ticket && t.ResumptionTicketLength > 0)
                parent.on_resumption_ticket(t.ResumptionTicket, t.ResumptionTicketLength);
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            LOG_INFO("Shutdown by transport: {:#x}",
                     (unsigned long)ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
            if (connecting) connect_failed_ = true;
            connecting = false; connected = false;
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            LOG_INFO("Shutdown by peer");
            if (connecting) connect_failed_ = true;
            connecting = false; connected = false;
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            LOG_INFO("Shutdown complete");
            if (connecting) connect_failed_ = true;
            connecting = false; connected = false;
            control_stream = nullptr; video_stream = nullptr;
            if (parent.on_disconnected) parent.on_disconnected();
            break;

        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS on_stream_event(HQUIC stream, QUIC_STREAM_EVENT* ev)
    {
        switch (ev->Type) {

        case QUIC_STREAM_EVENT_RECEIVE: {
            bool is_video = (stream == video_stream);
            for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; i++) {
                auto& b = ev->RECEIVE.Buffers[i];
                if (is_video) {
                    std::lock_guard lock(video_buffer_mutex);
                    parsing::process_video_stream_data(b.Buffer, b.Length,
                                                       video_recv_buffer,
                                                       parent.on_data_received);
                } else {
                    std::lock_guard lock(buffer_mutex);
                    parsing::process_stream_data(b.Buffer, b.Length,
                                                 recv_buffer, parent.incoming());
                }
            }
            break;
        }

        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            auto* qb = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
            if (qb) { delete[] qb->Buffer; delete qb; }
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (stream == control_stream) control_stream = nullptr;
            else if (stream == video_stream) video_stream = nullptr;
            api->StreamClose(stream);
            break;

        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

private:
    void cleanup_handles()
    {
        if (connection)    { api->ConnectionClose(connection);       connection    = nullptr; }
        if (configuration) { api->ConfigurationClose(configuration); configuration = nullptr; }
        if (registration)  { api->RegistrationClose(registration);   registration  = nullptr; }
    }
};

// ── File transfer stream context ──────────────────────────────────────────────

struct FileStreamCtx {
    NetClient* parent;
    const QUIC_API_TABLE* api;
    uint64_t attachment_id;
    bool is_upload;
    std::vector<uint8_t> recv_data;
};

static QUIC_STATUS QUIC_API s_file_stream_cb(HQUIC stream, void* ctx,
                                              QUIC_STREAM_EVENT* ev)
{
    auto* fctx = static_cast<FileStreamCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; i++) {
            auto& b = ev->RECEIVE.Buffers[i];
            fctx->recv_data.insert(fctx->recv_data.end(), b.Buffer, b.Buffer + b.Length);
        }
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Download complete — server finished sending file data
        if (!fctx->is_upload && fctx->parent->on_file_downloaded) {
            fctx->parent->on_file_downloaded(fctx->attachment_id,
                                              std::move(fctx->recv_data));
        }
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* qb = static_cast<QUIC_BUFFER*>(ev->SEND_COMPLETE.ClientContext);
        if (qb) { delete[] qb->Buffer; delete qb; }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        fctx->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        fctx->api->StreamClose(stream);
        delete fctx;
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

// ── NetClient method bodies ───────────────────────────────────────────────────

NetClient::NetClient()  : impl_(std::make_unique<Impl>(*this)) {}
NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const std::string& host, uint16_t port,
                        const uint8_t* ticket, size_t ticket_len)
{
    return impl_->connect(host, port, ticket, ticket_len);
}

void        NetClient::disconnect()              { impl_->disconnect(); }
std::string NetClient::get_server_fingerprint()  const { return impl_->server_fingerprint; }
bool        NetClient::is_connected()            const { return impl_->connected; }
bool        NetClient::is_connecting()           const { return impl_->connecting && !impl_->connected && !impl_->connect_failed_; }
bool        NetClient::connect_failed()          const { return impl_->connect_failed_; }

bool NetClient::send_message(protocol::ControlMessageType type,
                              const uint8_t* payload, size_t payload_len)
{
    return impl_->send_message(type, payload, payload_len);
}

bool NetClient::send_data(const uint8_t* data, size_t len, bool reliable)
{
    return impl_->send_data(data, len, reliable);
}

bool NetClient::send_video(const uint8_t* data, size_t len, bool reliable)
{
    return impl_->send_video(data, len, reliable);
}

// MsQuic opens both streams automatically on QUIC_CONNECTION_EVENT_CONNECTED.
void NetClient::open_av_streams() {}

bool NetClient::upload_file(uint64_t attachment_id, const uint8_t* data, size_t len)
{
    if (!impl_->connected || !impl_->connection) return false;

    auto* fctx = new FileStreamCtx{ this, impl_->api, attachment_id, true, {} };
    HQUIC stream = nullptr;

    if (QUIC_FAILED(impl_->api->StreamOpen(impl_->connection,
            QUIC_STREAM_OPEN_FLAG_NONE, s_file_stream_cb, fctx, &stream))) {
        delete fctx;
        return false;
    }
    if (QUIC_FAILED(impl_->api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        impl_->api->StreamClose(stream);
        delete fctx;
        return false;
    }

    // Send: [STREAM_TYPE_FILE_UPLOAD(1)][attachment_id(8)][file_data]
    size_t total = 1 + 8 + len;
    auto* buf = new uint8_t[total];
    buf[0] = protocol::STREAM_TYPE_FILE_UPLOAD;
    std::memcpy(buf + 1, &attachment_id, 8);
    std::memcpy(buf + 9, data, len);

    auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total), buf };
    if (QUIC_FAILED(impl_->api->StreamSend(stream, qb, 1, QUIC_SEND_FLAG_FIN, qb))) {
        delete[] buf; delete qb;
        return false;
    }
    return true;
}

bool NetClient::download_file(uint64_t attachment_id)
{
    if (!impl_->connected || !impl_->connection) return false;

    auto* fctx = new FileStreamCtx{ this, impl_->api, attachment_id, false, {} };
    HQUIC stream = nullptr;

    if (QUIC_FAILED(impl_->api->StreamOpen(impl_->connection,
            QUIC_STREAM_OPEN_FLAG_NONE, s_file_stream_cb, fctx, &stream))) {
        delete fctx;
        return false;
    }
    if (QUIC_FAILED(impl_->api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        impl_->api->StreamClose(stream);
        delete fctx;
        return false;
    }

    // Send: [STREAM_TYPE_FILE_DOWNLOAD(1)][attachment_id(8)] with FIN
    size_t total = 9;
    auto* buf = new uint8_t[total];
    buf[0] = protocol::STREAM_TYPE_FILE_DOWNLOAD;
    std::memcpy(buf + 1, &attachment_id, 8);

    auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total), buf };
    if (QUIC_FAILED(impl_->api->StreamSend(stream, qb, 1, QUIC_SEND_FLAG_FIN, qb))) {
        delete[] buf; delete qb;
        return false;
    }
    return true;
}

} // namespace parties::client
