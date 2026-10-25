#include <client/net_client.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>
#include <parties/protocol.h>
#include <parties/crypto.h>
#include <parties/video_common.h>

#include "net_client_parsing.h"

#include <parties/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace parties::client {

enum class SendKind : uint8_t {
    Control      = 0,
    VideoStream1 = 1,
    VideoFrame   = 2,
    Stream1Ctl   = 3,
};

struct SendCtx {
    QUIC_BUFFER buf{};
    uint8_t*    data = nullptr;
    uint32_t    len  = 0;
    SendKind    kind = SendKind::Control;
};

static SendCtx* new_send_ctx(size_t len, SendKind kind)
{
    auto* ctx = new SendCtx;
    ctx->data = new uint8_t[len];
    ctx->len  = static_cast<uint32_t>(len);
    ctx->kind = kind;
    ctx->buf  = QUIC_BUFFER{ ctx->len, ctx->data };
    return ctx;
}

static void free_send_ctx(SendCtx* ctx)
{
    delete[] ctx->data;
    delete ctx;
}

// Ownership: once StreamOpen succeeded the stream callback owns the context
// and frees it in SHUTDOWN_COMPLETE (StreamClose + delete). The sender makes
// exactly ONE further MsQuic call — StreamSend with QUIC_SEND_FLAG_START |
// QUIC_SEND_FLAG_FIN, which starts the stream and queues the frame in one
// operation — and never touches the context afterwards: MsQuic starts the
// stream asynchronously (with SHUTDOWN_ON_FAIL), so a start failure may
// already have delivered SHUTDOWN_COMPLETE on the worker thread by the time
// StreamSend returns. A synchronous StreamSend failure queues nothing, so the
// sender then asks for an ABORT|IMMEDIATE shutdown (asynchronous) whose
// SHUTDOWN_COMPLETE performs the cleanup. The sender never calls StreamClose.
struct VideoOutCtx {
    NetClient*            parent = nullptr;
    const QUIC_API_TABLE* api    = nullptr;
    QUIC_BUFFER           buf{};
    uint8_t*              data   = nullptr;
    uint32_t              len    = 0;
};

static QUIC_STATUS QUIC_API s_video_out_stream_cb(HQUIC stream, void* ctx,
                                                   QUIC_STREAM_EVENT* ev)
{
    auto* c = static_cast<VideoOutCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (c->parent->on_video_bytes_completed)
            c->parent->on_video_bytes_completed(c->len);
        delete[] c->data;
        c->data = nullptr;
        break;

    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        c->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Delivered inline from a StreamClose (AppCloseInProgress): the thread
        // inside StreamClose owns the handle and the context. The video path
        // never closes a live stream that way, but the guard makes a double
        // close / double free impossible.
        if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;
        c->api->StreamClose(stream);
        delete[] c->data;
        delete c;
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

struct VideoInCtx {
    NetClient*            parent = nullptr;
    const QUIC_API_TABLE* api    = nullptr;
    std::vector<uint8_t>  buf;
    bool     have_header = false;
    bool     dead        = false;
    bool     delivered   = false;
    uint32_t sender      = 0;
    uint32_t frame_seq   = 0;
};

constexpr size_t VIDEO_IN_PREFIX = 1 + 4;
static_assert(VIDEO_IN_PREFIX + VIDEO_FRAME_MAX_PAYLOAD_BYTES == VIDEO_FRAME_MAX_BYTES,
              "server video stream cap must equal the parser cap");

static void video_in_reject(VideoInCtx* c, HQUIC stream, const char* why)
{
    LOG_WARN("Rejecting server video stream ({}, {} bytes buffered)", why, c->buf.size());
    if (!c->delivered) {
        c->delivered = true;
        if (c->parent->on_video_stream_aborted)
            c->parent->on_video_stream_aborted(c->sender, c->frame_seq, c->have_header);
    }
    c->dead = true;
    std::vector<uint8_t>().swap(c->buf);
    c->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
}

static void video_in_complete(VideoInCtx* c)
{
    if (c->dead || c->delivered) return;
    c->delivered = true;
    if (!c->have_header || c->buf.size() < VIDEO_IN_PREFIX + VIDEO_FRAME_HEADER_SIZE) {
        LOG_WARN("Truncated server video stream ({} bytes)", c->buf.size());
        if (c->parent->on_video_stream_aborted)
            c->parent->on_video_stream_aborted(c->sender, c->frame_seq, c->have_header);
        std::vector<uint8_t>().swap(c->buf);
        return;
    }
    if (c->parent->on_video_stream_frame)
        c->parent->on_video_stream_frame(
            c->sender, std::vector<uint8_t>(c->buf.begin() + VIDEO_IN_PREFIX, c->buf.end()));
    std::vector<uint8_t>().swap(c->buf);
}

static QUIC_STATUS QUIC_API s_video_in_stream_cb(HQUIC stream, void* ctx,
                                                  QUIC_STREAM_EVENT* ev)
{
    auto* c = static_cast<VideoInCtx*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (c->dead) break;
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; i++) {
            auto& b = ev->RECEIVE.Buffers[i];
            c->buf.insert(c->buf.end(), b.Buffer, b.Buffer + b.Length);
        }
        if (!c->buf.empty() && c->buf[0] != protocol::STREAM_TYPE_VIDEO_FRAME) {
            video_in_reject(c, stream, "unexpected stream type");
            break;
        }
        if (c->buf.size() > VIDEO_IN_PREFIX + VIDEO_FRAME_MAX_PAYLOAD_BYTES) {
            video_in_reject(c, stream, "frame too large");
            break;
        }
        if (!c->have_header && c->buf.size() >= VIDEO_IN_PREFIX + VIDEO_FRAME_HEADER_SIZE) {
            std::memcpy(&c->sender, c->buf.data() + 1, 4);
            if (const auto hdr = VideoFrameHeader::parse(std::span(c->buf).subspan(VIDEO_IN_PREFIX))) {
                c->frame_seq   = hdr->frame_seq;
                c->have_header = true;
            }
        }
        if (ev->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN)
            video_in_complete(c);
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        video_in_complete(c);
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        if (!c->delivered) {
            c->delivered = true;
            if (c->parent->on_video_stream_aborted)
                c->parent->on_video_stream_aborted(c->sender, c->frame_seq, c->have_header);
        }
        c->dead = true;
        std::vector<uint8_t>().swap(c->buf);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Inline from a StreamClose (AppCloseInProgress): the closing thread
        // owns the handle and the context.
        if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;
        c->api->StreamClose(stream);
        delete c;
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

struct RejectedStreamCtx {
    const QUIC_API_TABLE* api = nullptr;
};

static QUIC_STATUS QUIC_API s_rejected_stream_cb(HQUIC stream, void* ctx,
                                                  QUIC_STREAM_EVENT* ev)
{
    auto* c = static_cast<RejectedStreamCtx*>(ctx);
    // Inline from a StreamClose (AppCloseInProgress): the closing thread owns
    // the handle and the context.
    if (ev->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE &&
        !ev->SHUTDOWN_COMPLETE.AppCloseInProgress) {
        c->api->StreamClose(stream);
        delete c;
    }
    return QUIC_STATUS_SUCCESS;
}

struct NetClient::Impl {
    NetClient& parent;

    const QUIC_API_TABLE* api  = nullptr;
    HQUIC registration         = nullptr;
    HQUIC configuration        = nullptr;
    HQUIC connection           = nullptr;
    std::atomic<HQUIC> control_stream{nullptr};
    std::atomic<HQUIC> video_stream{nullptr};

    std::shared_mutex handles_mutex;

    std::atomic<bool> connected{false};
    std::atomic<bool> connecting{false};
    std::atomic<bool> connect_failed_{false};
    std::mutex        write_mutex;

    std::mutex              shutdown_mutex;
    std::condition_variable shutdown_cv;
    bool                    shutdown_complete = false;

    std::mutex           buffer_mutex;
    std::vector<uint8_t> recv_buffer;
    std::mutex           video_buffer_mutex;
    std::vector<uint8_t> video_recv_buffer;

    std::string server_fingerprint;

    explicit Impl(NetClient& p) : parent(p) {}

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

        cleanup_handles();

        bool ok;
        {
            std::unique_lock<std::shared_mutex> lock(handles_mutex);
            { std::lock_guard<std::mutex> slock(shutdown_mutex); shutdown_complete = false; }
            ok = open_connection_locked(host, port, ticket, ticket_len);
        }
        if (!ok) {
            cleanup_handles();
            return false;
        }

        LOG_INFO("Connecting to {}:{} (MsQuic)...", host, port);
        return true;
    }

    bool open_connection_locked(const std::string& host, uint16_t port,
                                const uint8_t* ticket, size_t ticket_len)
    {
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
        // No send buffering: SEND_COMPLETE fires on ACK, which is what the
        // video backpressure accounting (on_video_bytes_completed) relies on.
        settings.SendBufferingEnabled   = FALSE; settings.IsSet.SendBufferingEnabled   = TRUE;
        settings.PacingEnabled          = TRUE;  settings.IsSet.PacingEnabled          = TRUE;
        // Per-frame video streams (1.2+): the server opens one unidirectional
        // stream per forwarded frame. The default 64 KB unidirectional receive
        // window would window-limit every keyframe.
        settings.PeerUnidiStreamCount   = 128;   settings.IsSet.PeerUnidiStreamCount   = TRUE;
        settings.StreamRecvWindowUnidiDefault = 4u * 1024u * 1024u;
        settings.IsSet.StreamRecvWindowUnidiDefault = TRUE;

        QUIC_BUFFER alpn = parties::make_alpn();
        status = api->ConfigurationOpen(registration, &alpn, 1, &settings,
                                        sizeof(settings), nullptr, &configuration);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConfigurationOpen: {:#x}", (unsigned long)status);
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
            return false;
        }

        status = api->ConnectionOpen(registration, s_connection_cb, this, &connection);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConnectionOpen: {:#x}", (unsigned long)status);
            return false;
        }

        if (ticket && ticket_len > 0) {
            status = api->SetParam(connection, QUIC_PARAM_CONN_RESUMPTION_TICKET,
                                   static_cast<uint32_t>(ticket_len), ticket);
            if (QUIC_FAILED(status))
                LOG_WARN("SetParam(RESUMPTION_TICKET): {:#x} (non-fatal)", (unsigned long)status);
        }

        // Armed before ConnectionStart: the worker may report the outcome
        // before this function returns.
        connecting      = true;
        connect_failed_ = false;
        status = api->ConnectionStart(connection, configuration,
                                      QUIC_ADDRESS_FAMILY_UNSPEC,
                                      host.c_str(), port);
        if (QUIC_FAILED(status)) {
            LOG_ERROR("ConnectionStart: {:#x}", (unsigned long)status);
            connecting = false;
            return false;
        }
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
            {
                std::shared_lock<std::shared_mutex> lock(handles_mutex);
                api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            }
            std::unique_lock<std::mutex> lock(shutdown_mutex);
            shutdown_cv.wait_for(lock, std::chrono::milliseconds(500),
                                 [this] { return shutdown_complete; });
        }

        cleanup_handles();

        { std::lock_guard lock(buffer_mutex);       recv_buffer.clear(); }
        { std::lock_guard lock(video_buffer_mutex); video_recv_buffer.clear(); }
        server_fingerprint.clear();
    }

    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len)
    {
        ZoneScopedN("NetClient::send_message");
        std::lock_guard<std::mutex> wlock(write_mutex);
        std::shared_lock<std::shared_mutex> hlock(handles_mutex);
        HQUIC cs = control_stream.load(std::memory_order_acquire);
        if (!connected || !cs) return false;

        const size_t total_len = 6 + payload_len;
        SendCtx* ctx = new_send_ctx(total_len, SendKind::Control);

        uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
        std::memcpy(ctx->data, &msg_len, 4);
        uint16_t t = static_cast<uint16_t>(type);
        std::memcpy(ctx->data + 4, &t, 2);
        if (payload_len > 0) std::memcpy(ctx->data + 6, payload, payload_len);

        if (QUIC_FAILED(api->StreamSend(cs, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx))) {
            free_send_ctx(ctx);
            return false;
        }
        return true;
    }

    bool send_data(const uint8_t* data, size_t len, bool priority)
    {
        ZoneScopedN("NetClient::send_data");
        std::shared_lock<std::shared_mutex> hlock(handles_mutex);
        if (!connected || !connection) return false;

        auto* buf = new uint8_t[len];
        std::memcpy(buf, data, len);
        auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf };
        const QUIC_SEND_FLAGS flags = priority ? QUIC_SEND_FLAG_DGRAM_PRIORITY
                                               : QUIC_SEND_FLAG_NONE;
        if (QUIC_FAILED(api->DatagramSend(connection, qb, 1, flags, qb))) {
            delete[] buf; delete qb; return false;
        }
        return true;
    }

    bool send_stream1(const uint8_t* header, size_t header_len,
                      const uint8_t* payload, size_t payload_len, SendKind kind)
    {
        ZoneScopedN("NetClient::send_video_parts");
        std::shared_lock<std::shared_mutex> hlock(handles_mutex);
        HQUIC vs = video_stream.load(std::memory_order_acquire);
        if (!connected || !vs) return false;

        const size_t len       = header_len + payload_len;
        const size_t total_len = 4 + len;
        SendCtx* ctx = new_send_ctx(total_len, kind);
        uint32_t flen = static_cast<uint32_t>(len);
        std::memcpy(ctx->data, &flen, 4);
        if (header_len)  std::memcpy(ctx->data + 4, header, header_len);
        if (payload_len) std::memcpy(ctx->data + 4 + header_len, payload, payload_len);

        if (QUIC_FAILED(api->StreamSend(vs, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx))) {
            free_send_ctx(ctx);
            return false;
        }
        return true;
    }

    bool send_video_parts(const uint8_t* header, size_t header_len,
                          const uint8_t* payload, size_t payload_len)
    {
        if (header_len + payload_len > VIDEO_FRAME_MAX_PAYLOAD_BYTES) return false;
        return send_stream1(header, header_len, payload, payload_len, SendKind::VideoStream1);
    }

    bool send_video(const uint8_t* data, size_t len)
    {
        return send_stream1(nullptr, 0, data, len, SendKind::Stream1Ctl);
    }

    // Stream credit: the server allows 128 concurrent per-frame streams. When
    // all of them are still open the start does NOT fail (no FAIL_BLOCKED):
    // MsQuic parks the stream until the server grants more credit, the bytes
    // stay charged in the caller's controller, and the sharer's admission gate
    // bounds how much can pile up behind the wait.
    bool send_video_frame_stream(const uint8_t* header, size_t header_len,
                                 const uint8_t* payload, size_t payload_len)
    {
        ZoneScopedN("NetClient::send_video_frame_stream");
        std::shared_lock<std::shared_mutex> hlock(handles_mutex);
        if (!connected || !connection) return false;

        if (header_len + payload_len > VIDEO_FRAME_MAX_PAYLOAD_BYTES) return false;
        const size_t total = 1 + header_len + payload_len;

        auto* ctx = new VideoOutCtx;
        ctx->parent = &parent;
        ctx->api    = api;
        ctx->data   = new uint8_t[total];
        ctx->len    = static_cast<uint32_t>(total);
        ctx->data[0] = protocol::STREAM_TYPE_VIDEO_FRAME;
        if (header_len)  std::memcpy(ctx->data + 1, header, header_len);
        if (payload_len) std::memcpy(ctx->data + 1 + header_len, payload, payload_len);
        ctx->buf = QUIC_BUFFER{ ctx->len, ctx->data };

        HQUIC stream = nullptr;
        if (QUIC_FAILED(api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                        s_video_out_stream_cb, ctx, &stream))) {
            delete[] ctx->data; delete ctx;
            return false;
        }

        // Start + send + FIN in one call. The start is asynchronous (with
        // SHUTDOWN_ON_FAIL): if it fails — e.g. the connection is already
        // closing — the worker cancels the queued send (SEND_COMPLETE with
        // Canceled keeps the accounting balanced) and delivers
        // SHUTDOWN_COMPLETE, which closes the handle and frees ctx. That may
        // already have happened by the time this call returns, so ctx is never
        // touched afterwards.
        if (QUIC_FAILED(api->StreamSend(stream, &ctx->buf, 1,
                                        QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, ctx))) {
            // Nothing was queued (no SEND_COMPLETE will fire for it). Hand the
            // stream to an asynchronous abort whose SHUTDOWN_COMPLETE closes
            // the handle and frees ctx; neither may be touched from here on.
            api->StreamShutdown(stream,
                QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE, 0);
            return false;
        }
        return true;
    }

    NetClient::ConnectionStats connection_stats()
    {
        NetClient::ConnectionStats out;
        std::shared_lock<std::shared_mutex> hlock(handles_mutex);
        if (!connected || !connection) return out;

        QUIC_STATISTICS_V2 stats{};
        uint32_t size = sizeof(stats);
        if (QUIC_FAILED(api->GetParam(connection, QUIC_PARAM_CONN_STATISTICS_V2, &size, &stats)))
            return out;

        out.valid                  = true;
        out.rtt_us                 = stats.Rtt;
        out.min_rtt_us             = stats.MinRtt;
        out.sent_packets           = stats.SendTotalPackets;
        out.suspected_lost_packets = stats.SendSuspectedLostPackets;
        out.spurious_lost_packets  = stats.SendSpuriousLostPackets;
        out.sent_bytes             = stats.SendTotalBytes;
        out.congestion_window      = stats.SendCongestionWindow;
        return out;
    }

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

            HQUIC stream = nullptr;
            if (QUIC_SUCCEEDED(api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                               s_stream_cb, this, &stream)) &&
                QUIC_SUCCEEDED(api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
                control_stream = stream;
            } else {
                LOG_ERROR("Control stream failed");
                if (stream) api->StreamClose(stream);
            }

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

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            HQUIC s = ev->PEER_STREAM_STARTED.Stream;
            if (ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) {
                auto* vctx = new VideoInCtx;
                vctx->parent = &parent;
                vctx->api    = api;
                api->SetCallbackHandler(s, (void*)s_video_in_stream_cb, vctx);
            } else {
                LOG_WARN("Unexpected bidirectional stream from server; aborting it");
                auto* rctx = new RejectedStreamCtx{ api };
                api->SetCallbackHandler(s, (void*)s_rejected_stream_cb, rctx);
                api->StreamShutdown(s, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
            auto* cert = static_cast<QUIC_BUFFER*>(ev->PEER_CERTIFICATE_RECEIVED.Certificate);
            if (cert && cert->Buffer && cert->Length > 0) {
                server_fingerprint = parties::sha256_colon_hex(cert->Buffer, cert->Length);
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
            { std::lock_guard lock(shutdown_mutex); shutdown_complete = true; }
            shutdown_cv.notify_all();
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
            auto* ctx = static_cast<SendCtx*>(ev->SEND_COMPLETE.ClientContext);
            if (ctx) {
                if (ctx->kind == SendKind::VideoStream1 && parent.on_video_bytes_completed)
                    parent.on_video_bytes_completed(ctx->len);
                free_send_ctx(ctx);
            }
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (stream == control_stream) control_stream = nullptr;
            else if (stream == video_stream) video_stream = nullptr;
            // Delivered inline from our own StreamClose (a stream whose start
            // failed in CONNECTED): that call owns the handle.
            if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;
            api->StreamClose(stream);
            break;

        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

private:
    // Detaches the QUIC handles under the exclusive lock, then closes them
    // with the lock released: ConnectionClose waits for in-flight callbacks,
    // and a callback may re-enter a send path that takes the lock shared
    // (holding the lock across the close would deadlock). After the detach no
    // sender can observe the old handles, so the close is race-free.
    void cleanup_handles()
    {
        HQUIC conn, cfg, reg;
        {
            std::unique_lock<std::shared_mutex> lock(handles_mutex);
            conn = connection;    connection    = nullptr;
            cfg  = configuration; configuration = nullptr;
            reg  = registration;  registration  = nullptr;
            control_stream = nullptr;
            video_stream   = nullptr;
        }
        if (conn) api->ConnectionClose(conn);
        if (cfg)  api->ConfigurationClose(cfg);
        if (reg)  api->RegistrationClose(reg);
    }
};

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
        // Delivered inline from StreamClose (AppCloseInProgress) on the
        // upload_file / download_file start-failure paths: the caller owns the
        // handle and frees fctx itself.
        if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;
        fctx->api->StreamClose(stream);
        delete fctx;
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

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

bool NetClient::send_data(const uint8_t* data, size_t len, bool priority)
{
    return impl_->send_data(data, len, priority);
}

bool NetClient::send_video(const uint8_t* data, size_t len)
{
    return impl_->send_video(data, len);
}

bool NetClient::send_video_parts(const uint8_t* header, size_t header_len,
                                 const uint8_t* payload, size_t payload_len)
{
    return impl_->send_video_parts(header, header_len, payload, payload_len);
}

bool NetClient::send_video_frame_stream(const uint8_t* header, size_t header_len,
                                        const uint8_t* payload, size_t payload_len)
{
    return impl_->send_video_frame_stream(header, header_len, payload, payload_len);
}

NetClient::ConnectionStats NetClient::connection_stats() const
{
    return impl_->connection_stats();
}


bool NetClient::upload_file(uint64_t attachment_id, const uint8_t* data, size_t len)
{
    std::shared_lock<std::shared_mutex> hlock(impl_->handles_mutex);
    if (!impl_->connected || !impl_->connection) return false;

    auto* fctx = new FileStreamCtx{ this, impl_->api, attachment_id, true, {} };
    HQUIC stream = nullptr;

    if (QUIC_FAILED(impl_->api->StreamOpen(impl_->connection,
            QUIC_STREAM_OPEN_FLAG_NONE, s_file_stream_cb, fctx, &stream))) {
        delete fctx;
        return false;
    }
    if (QUIC_FAILED(impl_->api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        // StreamClose delivers SHUTDOWN_COMPLETE inline with AppCloseInProgress
        // set; the callback leaves the handle and fctx to us.
        impl_->api->StreamClose(stream);
        delete fctx;
        return false;
    }

    size_t total = 1 + 8 + len;
    auto* buf = new uint8_t[total];
    buf[0] = protocol::STREAM_TYPE_FILE_UPLOAD;
    std::memcpy(buf + 1, &attachment_id, 8);
    std::memcpy(buf + 9, data, len);

    auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total), buf };
    if (QUIC_FAILED(impl_->api->StreamSend(stream, qb, 1, QUIC_SEND_FLAG_FIN, qb))) {
        delete[] buf; delete qb;
        impl_->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        return false;
    }
    return true;
}

bool NetClient::download_file(uint64_t attachment_id)
{
    std::shared_lock<std::shared_mutex> hlock(impl_->handles_mutex);
    if (!impl_->connected || !impl_->connection) return false;

    auto* fctx = new FileStreamCtx{ this, impl_->api, attachment_id, false, {} };
    HQUIC stream = nullptr;

    if (QUIC_FAILED(impl_->api->StreamOpen(impl_->connection,
            QUIC_STREAM_OPEN_FLAG_NONE, s_file_stream_cb, fctx, &stream))) {
        delete fctx;
        return false;
    }
    if (QUIC_FAILED(impl_->api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        // StreamClose delivers SHUTDOWN_COMPLETE inline with AppCloseInProgress
        // set; the callback leaves the handle and fctx to us.
        impl_->api->StreamClose(stream);
        delete fctx;
        return false;
    }

    size_t total = 9;
    auto* buf = new uint8_t[total];
    buf[0] = protocol::STREAM_TYPE_FILE_DOWNLOAD;
    std::memcpy(buf + 1, &attachment_id, 8);

    auto* qb = new QUIC_BUFFER{ static_cast<uint32_t>(total), buf };
    if (QUIC_FAILED(impl_->api->StreamSend(stream, qb, 1, QUIC_SEND_FLAG_FIN, qb))) {
        delete[] buf; delete qb;
        impl_->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        return false;
    }
    return true;
}

}
