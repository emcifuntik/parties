#include <server/quic_server.h>
#include <parties/quic_common.h>
#include <parties/protocol.h>
#include <parties/server_query.h>
#include <parties/profiler.h>

#include <parties/log.h>
#include <parties/video_common.h>
#include <atomic>
#include <chrono>
#include <cstring>

namespace parties::server {

// ── Locking contract ─────────────────────────────────────────────────────────
// sessions_mutex_      guards the session map only.
// Session::quic_mutex  serializes every use of that session's QUIC handles
//                      (StreamOpen/Start/Send/DatagramSend) against their
//                      closure in the connection's SHUTDOWN_COMPLETE, which
//                      takes quic_mutex, sets alive=false, nulls the stream
//                      handles and only then closes the connection.
// Order: sessions_mutex_ MAY be held while taking quic_mutex (send_to,
// broadcast, SHUTDOWN_COMPLETE); quic_mutex is NEVER held while taking
// sessions_mutex_. Data-plane senders on the MsQuic thread
// (send_video_frame_to) take only quic_mutex.
// Send contexts own a shared_ptr<Session>, so a completion callback that fires
// after the session was erased from the map still finds a live object.

// Context stored in QUIC connection/stream user data
struct ConnectionContext {
    QuicServer* server;
    uint32_t session_id;
};

// Context of one StreamSend on a long-lived stream (control stream, legacy
// video stream 1, file download stream). MsQuic keeps only the QUIC_BUFFER
// pointers until SEND_COMPLETE, so the descriptors and the bytes live here.
struct SendCtx {
    static constexpr uint8_t KIND_CONTROL = 0;
    static constexpr uint8_t KIND_VIDEO   = 1;

    QUIC_BUFFER bufs[2]{};
    uint32_t    buf_count = 0;
    std::vector<uint8_t> owned;                              // prefix / whole message
    std::shared_ptr<const std::vector<uint8_t>> shared;      // frame body shared by all viewers
    std::shared_ptr<Session> session;                        // set for KIND_VIDEO accounting
    uint32_t    len  = 0;                                    // bytes charged to video_outstanding_bytes
    uint8_t     kind = KIND_CONTROL;
};

// Completes the video accounting of a SendCtx exactly once.
static void send_ctx_uncharge(SendCtx* ctx) {
    if (ctx->kind == SendCtx::KIND_VIDEO && ctx->session && ctx->len) {
        ctx->session->video_outstanding_bytes.fetch_sub(ctx->len, std::memory_order_relaxed);
        ctx->len = 0;
    }
}

// Coalesces a peer-triggered log line to one per second per call site: a
// misbehaving peer can trigger these far faster than they are worth logging.
// allow() returns true when a line may be written now; `suppressed` receives
// the number of events swallowed since the previous line.
struct LogThrottle {
    std::atomic<int64_t>  last_us{0};
    std::atomic<uint32_t> suppressed_count{0};

    bool allow(uint32_t& suppressed) {
        const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_us.load(std::memory_order_relaxed);
        if ((last != 0 && now - last < 1'000'000) ||
            !last_us.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            suppressed_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        suppressed = suppressed_count.exchange(0, std::memory_order_relaxed);
        return true;
    }
};

static LogThrottle video_reject_log_throttle;    // refused per-frame streams
static LogThrottle video_ingress_log_throttle;   // malformed / oversize / over-cap streams

// Peer-opened unidirectional stream refused at PEER_STREAM_STARTED (session
// unknown, unauthenticated or not an active sharer): aborted on arrival. This
// context exists only so SHUTDOWN_COMPLETE can close the handle.
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

// Inbound per-frame unidirectional stream (sharer -> server). Every byte it
// buffers is charged to Session::video_in_buffered_bytes (capped per session
// at VIDEO_INGRESS_MAX_BUFFERED_BYTES) until the frame is delivered or the
// stream is given up on, so unfinished streams cannot pin unbounded memory.
struct QuicServer::VideoInStreamContext {
    QuicServer* server = nullptr;
    uint32_t    session_id = 0;
    std::shared_ptr<Session> session;   // ingress accounting (video_in_buffered_bytes)
    std::vector<uint8_t> buf;           // [STREAM_TYPE_VIDEO_FRAME][header][encoded] accumulated until FIN
    int64_t     charged = 0;            // bytes of this stream counted in session->video_in_buffered_bytes
    bool        dead = false;           // malformed/oversize/over cap: aborted, ignore further data

    // Returns this stream's bytes to the session's ingress budget exactly once.
    void uncharge() {
        if (charged && session)
            session->video_in_buffered_bytes.fetch_sub(charged, std::memory_order_relaxed);
        charged = 0;
    }

    // Gives up on this stream: releases the accumulator and its budget and
    // aborts the stream (SHUTDOWN_COMPLETE follows and frees the context).
    void abort(HQUIC stream) {
        if (dead) return;
        dead = true;
        std::vector<uint8_t>().swap(buf);
        uncharge();
        server->api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
};

// Outbound per-frame unidirectional stream (server -> viewer).
//
// Ownership: once StreamOpen succeeded the stream callback owns the context
// and frees it in SHUTDOWN_COMPLETE (StreamClose + delete). The sender makes
// exactly ONE further MsQuic call — StreamSend with QUIC_SEND_FLAG_START|FIN,
// which starts the stream and queues the frame atomically — and never touches
// the context afterwards: MsQuic starts the stream asynchronously (with
// SHUTDOWN_ON_FAIL), so a start failure may already have delivered
// SHUTDOWN_COMPLETE, inline or on the viewer's worker, by the time StreamSend
// returns. A synchronous StreamSend failure queues nothing, so the sender then
// asks for an ABORT|IMMEDIATE shutdown (asynchronous, never blocks) whose
// SHUTDOWN_COMPLETE performs the cleanup. The sender therefore never calls
// StreamClose — which would block on the viewer's worker while quic_mutex is
// held — from a foreign thread.
struct QuicServer::VideoOutStreamContext {
    QuicServer* server = nullptr;
    std::shared_ptr<Session> session;
    std::shared_ptr<const std::vector<uint8_t>> frame;
    uint8_t     type_byte = protocol::STREAM_TYPE_VIDEO_FRAME;
    QUIC_BUFFER bufs[2]{};
    std::atomic<uint32_t> charged{0};   // bytes still counted in video_outstanding_bytes

    // Settles the viewer's outstanding-bytes accounting exactly once.
    void uncharge() {
        uint32_t n = charged.exchange(0, std::memory_order_acq_rel);
        if (n && session)
            session->video_outstanding_bytes.fetch_sub(n, std::memory_order_relaxed);
    }
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
            std::lock_guard<std::mutex> qlock(session->quic_mutex);
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
    if (it == sessions_.end()) return false;

    auto& session = it->second;
    // quic_mutex nested inside sessions_mutex_ (allowed order) so the control
    // stream handle can't be closed underneath the send.
    std::lock_guard<std::mutex> qlock(session->quic_mutex);
    if (!session->alive || !session->quic_control_stream) return false;

    return send_control_on_stream(session->quic_control_stream, type, payload, payload_len);
}

void QuicServer::broadcast(protocol::ControlMessageType type,
                            const uint8_t* payload, size_t payload_len) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        std::lock_guard<std::mutex> qlock(session->quic_mutex);
        if (session->alive && session->authenticated && session->quic_control_stream) {
            send_control_on_stream(session->quic_control_stream, type, payload, payload_len);
        }
    }
}

void QuicServer::disconnect(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;

    std::lock_guard<std::mutex> qlock(it->second->quic_mutex);
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

bool QuicServer::query_min_rtt(const std::shared_ptr<Session>& session, uint32_t& min_rtt_us) {
    if (!session) return false;

    // quic_mutex keeps the connection handle from being closed underneath
    // GetParam (SHUTDOWN_COMPLETE nulls it under the same lock).
    std::lock_guard<std::mutex> qlock(session->quic_mutex);
    if (!session->alive || !session->quic_connection) return false;

    QUIC_STATISTICS_V2 stats{};
    uint32_t size = sizeof(stats);
    if (QUIC_FAILED(api_->GetParam(session->quic_connection, QUIC_PARAM_CONN_STATISTICS_V2,
                                   &size, &stats)))
        return false;
    min_rtt_us = stats.MinRtt;
    return true;
}

// ── Data plane ──

bool QuicServer::send_datagram(uint32_t session_id, const uint8_t* data, size_t len,
                               bool priority) {
	ZoneScopedN("QuicServer::send_datagram");
    std::shared_ptr<Session> session = get_session(session_id);
    if (!session) return false;

    std::lock_guard<std::mutex> qlock(session->quic_mutex);
    if (!session->alive || !session->quic_connection)
        return false;

    // Both the data buffer AND the QUIC_BUFFER descriptor must be heap-allocated
    // because DatagramSend is async — MsQuic stores only the pointer.
    auto* buf_data = new uint8_t[len];
    std::memcpy(buf_data, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf_data };
    QUIC_STATUS status = api_->DatagramSend(session->quic_connection,
        quic_buf, 1, priority ? QUIC_SEND_FLAG_DGRAM_PRIORITY : QUIC_SEND_FLAG_NONE,
        quic_buf);

    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

void QuicServer::send_to_many(const std::vector<uint32_t>& session_ids,
                               const uint8_t* data, size_t len, bool priority) {
	ZoneScopedN("QuicServer::send_to_many");
    for (uint32_t id : session_ids)
        send_datagram(id, data, len, priority);
}

bool QuicServer::send_stream(uint32_t session_id, const uint8_t* data, size_t len) {
    return send_video_to(session_id, data, len);
}

bool QuicServer::send_video_to(uint32_t session_id, const uint8_t* data, size_t len) {
	ZoneScopedN("QuicServer::send_video_to");
    std::shared_ptr<Session> session = get_session(session_id);
    if (!session) return false;

    // Wire format: [u32 len][data]
    auto* ctx = new SendCtx;
    ctx->kind = SendCtx::KIND_VIDEO;
    ctx->session = session;
    ctx->owned.resize(4 + len);
    uint32_t frame_len = static_cast<uint32_t>(len);
    std::memcpy(ctx->owned.data(), &frame_len, 4);
    if (len) std::memcpy(ctx->owned.data() + 4, data, len);
    ctx->bufs[0] = QUIC_BUFFER{ static_cast<uint32_t>(ctx->owned.size()), ctx->owned.data() };
    ctx->buf_count = 1;
    ctx->len = static_cast<uint32_t>(ctx->owned.size());

    std::lock_guard<std::mutex> qlock(session->quic_mutex);
    if (!session->alive || !session->quic_video_stream) {
        delete ctx;
        return false;
    }

    // Charge before the send so a completion racing ahead never underflows.
    session->video_outstanding_bytes.fetch_add(ctx->len, std::memory_order_relaxed);
    QUIC_STATUS status = api_->StreamSend(session->quic_video_stream, ctx->bufs, ctx->buf_count,
                                           QUIC_SEND_FLAG_NONE, ctx);
    if (QUIC_FAILED(status)) {
        send_ctx_uncharge(ctx);
        delete ctx;
        reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool QuicServer::send_video_frame_to(const std::shared_ptr<Session>& session,
                                     std::shared_ptr<const std::vector<uint8_t>> frame) {
	ZoneScopedN("QuicServer::send_video_frame_to");
    if (!session || !frame) return false;

    std::lock_guard<std::mutex> qlock(session->quic_mutex);
    if (!session->alive || !session->quic_connection) return false;

    // Belt and braces: every ingress caps sharer frames at
    // VIDEO_FRAME_MAX_PAYLOAD_BYTES, so with the type byte (and the sender id
    // already inside `frame`) the on-wire record always fits the receivers'
    // VIDEO_FRAME_MAX_BYTES parser cap. Should the two limits ever desync,
    // drop the frame rather than break a legacy viewer's stream-1 parser (or
    // have a 1.2 viewer abort the stream).
    if (1 + frame->size() > VIDEO_FRAME_MAX_BYTES) return false;

    if (session->supports_frame_streams()) {
        // ── Per-frame unidirectional stream: [0x12][frame] + FIN ──
        auto* ctx = new VideoOutStreamContext;
        ctx->server  = this;
        ctx->session = session;
        ctx->frame   = std::move(frame);
        ctx->bufs[0] = QUIC_BUFFER{ 1, &ctx->type_byte };
        ctx->bufs[1] = QUIC_BUFFER{ static_cast<uint32_t>(ctx->frame->size()),
                                    const_cast<uint8_t*>(ctx->frame->data()) };

        HQUIC stream = nullptr;
        QUIC_STATUS status = api_->StreamOpen(session->quic_connection,
                                              QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                              video_out_stream_callback, ctx, &stream);
        if (QUIC_FAILED(status)) {
            // No handle, no callbacks: nothing else owns the context.
            delete ctx;
            reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Charge before the send so a completion racing ahead never underflows.
        const uint32_t total = 1 + static_cast<uint32_t>(ctx->frame->size());
        session->video_outstanding_bytes.fetch_add(total, std::memory_order_relaxed);
        ctx->charged.store(total, std::memory_order_release);

        // Start + send + FIN in one call (see VideoOutStreamContext). The
        // start is asynchronous with SHUTDOWN_ON_FAIL and WITHOUT FAIL_BLOCKED:
        // when the viewer's unidirectional stream credit is exhausted (its 128
        // per-frame streams are all still open) MsQuic parks the stream in
        // WaitingStreams until an older one closes — the start does not fail,
        // the bytes stay charged to video_outstanding_bytes, and the backlog
        // gate bounds the wait. A start fails only for a closed connection or
        // an allocation failure, and then ends in SHUTDOWN_COMPLETE with the
        // queued send completed as canceled.
        status = api_->StreamSend(stream, ctx->bufs, 2,
                                  QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, ctx);
        if (QUIC_FAILED(status)) {
            // Nothing was queued and the stream never started, so no callback
            // has run yet: undo the accounting, then hand the stream to an
            // asynchronous abort whose SHUTDOWN_COMPLETE closes it and frees
            // the context. Do not touch `ctx` after this point.
            ctx->uncharge();
            api_->StreamShutdown(stream,
                QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE, 0);
            reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // The stream callback owns the context from here on.
        return true;
    }

    // ── Legacy viewer: [u32 len][VIDEO_FRAME_PACKET_TYPE][frame] on stream 1 ──
    if (!session->quic_video_stream) return false;

    auto* ctx = new SendCtx;
    ctx->kind = SendCtx::KIND_VIDEO;
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

    session->video_outstanding_bytes.fetch_add(ctx->len, std::memory_order_relaxed);
    QUIC_STATUS status = api_->StreamSend(session->quic_video_stream, ctx->bufs, ctx->buf_count,
                                           QUIC_SEND_FLAG_NONE, ctx);
    if (QUIC_FAILED(status)) {
        send_ctx_uncharge(ctx);
        delete ctx;
        reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
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

    // The MsQuic patch only raises this for datagrams matching the magic, but
    // re-validate defensively before replying.
    if (!parties::is_server_query_request(q.Payload, q.PayloadLength)) {
        q.ReplyBufferLength = 0;
        return QUIC_STATUS_SUCCESS;
    }

    parties::ServerQueryInfo info;
    info.protocol_version = protocol::PROTOCOL_VERSION;
    info.server_name      = query_server_name_;
    info.max_users        = query_max_users_;
    info.password_locked  = query_password_locked_;

    uint16_t count = 0;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [id, session] : sessions_) {
            if (session->authenticated) ++count;
        }
    }
    info.current_users = count;

    uint32_t token = parties::server_query_request_token(q.Payload, q.PayloadLength);
    std::vector<uint8_t> reply = parties::build_server_query_reply(token, info);

    // Anti-amplification: the reply buffer capacity equals the request length.
    // If the reply somehow doesn't fit, send nothing rather than truncate.
    if (reply.size() > q.ReplyBufferLength) {
        q.ReplyBufferLength = 0;
        return QUIC_STATUS_SUCCESS;
    }

    std::memcpy(q.ReplyBuffer, reply.data(), reply.size());
    q.ReplyBufferLength = static_cast<uint16_t>(reply.size());
    return QUIC_STATUS_SUCCESS;
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
        HQUIC stream = event->PEER_STREAM_STARTED.Stream;

        // Unidirectional = one per-frame video stream (protocol 1.2). It is
        // never one of the positional bidirectional streams below.
        if (event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) {
            // Only a session the main loop registered as an active sharer may
            // buffer video here (Session::video_ingress_allowed). Anything
            // else — unknown session, unauthenticated peer, idle user — is
            // aborted on arrival so it cannot pin memory with unfinished
            // frames; a minimal context just closes the handle afterwards.
            // (The acquire load orders the plain `authenticated` read after
            // the main loop's release store of the flag.)
            std::shared_ptr<Session> session;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = sessions_.find(session_id);
                if (it != sessions_.end()) session = it->second;
            }
            const bool allowed = session &&
                                 session->video_ingress_allowed.load(std::memory_order_acquire) &&
                                 session->authenticated;
            if (!allowed) {
                uint32_t suppressed = 0;
                if (video_reject_log_throttle.allow(suppressed)) {
                    LOG_DEBUG("Session {}: refused per-frame video stream (not an active sharer); {} more refusal(s) since the last line",
                              session_id, suppressed);
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

        // Client opened a bidirectional stream: control, then video 1, then
        // file transfers (positional).
        bool is_file_stream = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                std::lock_guard<std::mutex> qlock(it->second->quic_mutex);
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

        // Capture the fields the main loop needs to broadcast the departure,
        // then hand off via the queue. Doing this under sessions_mutex_ (and
        // never mutating Session state here beyond the transport handles)
        // keeps all disconnect-time Session access on the main thread,
        // eliminating the worker/main-loop race.
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                auto& session = it->second;
                // Retire the QUIC handles under quic_mutex: any sender that
                // still holds a shared_ptr<Session> (send contexts, the
                // forwarding fast path) sees alive=false and null handles
                // before ConnectionClose runs below.
                {
                    std::lock_guard<std::mutex> qlock(session->quic_mutex);
                    session->alive = false;
                    session->quic_connection = nullptr;
                    session->quic_control_stream = nullptr;
                    session->quic_video_stream = nullptr;
                }
                if (session->authenticated) {
                    disconnects_.push(SessionDisconnect{
                        session_id, session->user_id, session->channel_id});
                }
                sessions_.erase(it);
            }
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
        if (auto session = get_session(session_id)) {
            std::lock_guard<std::mutex> qlock(session->quic_mutex);
            is_video = (stream == session->quic_video_stream);
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
        // Free the SendCtx allocated by send_control_on_stream / send_video_to /
        // send_video_frame_to (legacy path); video kinds also settle the
        // viewer's outstanding-bytes accounting (fires on ACK because send
        // buffering is disabled).
        auto* ctx = static_cast<SendCtx*>(event->SEND_COMPLETE.ClientContext);
        if (ctx) {
            send_ctx_uncharge(ctx);
            delete ctx;
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        // Retire the handle under quic_mutex BEFORE closing it so no sender
        // can pick it up in between.
        if (auto session = get_session(session_id)) {
            std::lock_guard<std::mutex> qlock(session->quic_mutex);
            if (session->quic_control_stream == stream)
                session->quic_control_stream = nullptr;
            else if (session->quic_video_stream == stream)
                session->quic_video_stream = nullptr;
        }
        api_->StreamClose(stream);
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

    // MsQuic keeps only the QUIC_BUFFER pointer until SEND_COMPLETE, so the
    // descriptor and the bytes live in a SendCtx freed there.
    auto* ctx = new SendCtx;
    ctx->kind = SendCtx::KIND_CONTROL;
    ctx->owned.resize(total_len);
    std::memcpy(ctx->owned.data(), &msg_len, 4);
    uint16_t msg_type = static_cast<uint16_t>(type);
    std::memcpy(ctx->owned.data() + 4, &msg_type, 2);
    if (payload_len > 0)
        std::memcpy(ctx->owned.data() + 6, payload, payload_len);
    ctx->bufs[0] = QUIC_BUFFER{ static_cast<uint32_t>(total_len), ctx->owned.data() };
    ctx->buf_count = 1;

    QUIC_STATUS status = api_->StreamSend(stream, ctx->bufs, ctx->buf_count,
                                           QUIC_SEND_FLAG_NONE, ctx);
    if (QUIC_FAILED(status)) {
        delete ctx;
        reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
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

            // Same cap as the per-frame ingress: [type][header][encoded] with
            // the sharer's payload limited to VIDEO_FRAME_MAX_PAYLOAD_BYTES, so
            // every re-originated record fits the receivers' parser cap.
            if (frame_len == 0 || frame_len > 1 + VIDEO_FRAME_MAX_PAYLOAD_BYTES) {
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
        auto* sctx = static_cast<SendCtx*>(event->SEND_COMPLETE.ClientContext);
        if (sctx) {
            send_ctx_uncharge(sctx);
            delete sctx;
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
    auto* ctx = new SendCtx;
    ctx->kind = SendCtx::KIND_CONTROL;
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

// ── Per-frame video streams ──

// Inbound: sharer -> server. One whole frame per stream:
// [STREAM_TYPE_VIDEO_FRAME][VideoFrameHeader 14][encoded] + FIN.
QUIC_STATUS QUIC_API QuicServer::video_in_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<VideoInStreamContext*>(context);
    QuicServer* self = ctx->server;
    const QUIC_API_TABLE* api = self->api_;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (ctx->dead) break;   // already aborted; data is consumed and ignored

        // Charge the bytes to the session's ingress budget BEFORE buffering
        // them: a peer that opens streams and never finishes them can hold at
        // most VIDEO_INGRESS_MAX_BUFFERED_BYTES on the server, whatever its
        // stream credit.
        int64_t incoming = 0;
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++)
            incoming += event->RECEIVE.Buffers[i].Length;
        ctx->charged += incoming;
        const int64_t buffered = ctx->session->video_in_buffered_bytes.fetch_add(
            incoming, std::memory_order_relaxed) + incoming;
        if (buffered > Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES) {
            uint32_t suppressed = 0;
            if (video_ingress_log_throttle.allow(suppressed)) {
                LOG_WARN("Session {}: per-frame video ingress over the {} byte buffer cap ({} buffered) - aborting stream; {} more since the last line",
                         ctx->session_id, Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES, buffered, suppressed);
            }
            ctx->abort(stream);
            break;
        }

        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& b = event->RECEIVE.Buffers[i];
            ctx->buf.insert(ctx->buf.end(), b.Buffer, b.Buffer + b.Length);
        }

        // Validate as early as the first byte: anything that is not a video
        // frame, or that grows past the largest payload a sharer may send, is
        // aborted and its accumulator (and budget share) released.
        if ((!ctx->buf.empty() && ctx->buf[0] != protocol::STREAM_TYPE_VIDEO_FRAME) ||
            ctx->buf.size() > 1 + VIDEO_FRAME_MAX_PAYLOAD_BYTES) {
            uint32_t suppressed = 0;
            if (video_ingress_log_throttle.allow(suppressed)) {
                LOG_WARN("Session {}: invalid per-frame video stream (first byte {:#x}, {} bytes) - aborting; {} more since the last line",
                         ctx->session_id, ctx->buf.empty() ? 0u : (unsigned)ctx->buf[0], ctx->buf.size(), suppressed);
            }
            ctx->abort(stream);
            break;
        }

        if (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) {
            if (ctx->buf.size() >= 1 + VIDEO_FRAME_HEADER_SIZE) {
                const uint8_t* frame = ctx->buf.data() + 1;
                const size_t   flen  = ctx->buf.size() - 1;
                if (self->on_video_frame) {
                    self->on_video_frame(ctx->session_id, protocol::VIDEO_FRAME_PACKET_TYPE,
                                         frame, flen);
                } else {
                    DataPacket pkt;
                    pkt.session_id = ctx->session_id;
                    pkt.packet_type = protocol::VIDEO_FRAME_PACKET_TYPE;
                    pkt.channel_id = 0;
                    pkt.reliable = true;
                    pkt.data.assign(frame, frame + flen);
                    self->data_incoming_.push(std::move(pkt));
                }
            } else {
                LOG_DEBUG("Session {}: dropping truncated per-frame video stream ({} bytes)",
                          ctx->session_id, ctx->buf.size());
            }
            // Frame delivered (on_video_frame copies what it forwards):
            // release the accumulator and its share of the ingress budget.
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
        // The sharer gave up on this frame (or died): release everything.
        ctx->abort(stream);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Final event: settle the ingress accounting (a no-op unless the
        // stream ended without FIN or abort), close the handle and free the
        // context.
        ctx->uncharge();
        api->StreamClose(stream);
        delete ctx;
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// Outbound: server -> viewer. See VideoOutStreamContext for ownership.
QUIC_STATUS QUIC_API QuicServer::video_out_stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* ctx = static_cast<VideoOutStreamContext*>(context);
    const QUIC_API_TABLE* api = ctx->server->api_;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event->START_COMPLETE.Status)) {
            // Only a closed connection or an allocation failure gets here —
            // never STREAM_LIMIT_REACHED, which requires FAIL_BLOCKED (without
            // it MsQuic parks the stream in WaitingStreams until the viewer
            // closes an older frame stream, its bytes still charged to the
            // backlog gate). SHUTDOWN_ON_FAIL guarantees SHUTDOWN_COMPLETE
            // follows (queued sends complete as canceled first).
            LOG_DEBUG("Per-frame video stream start failed: {:#x}",
                      (unsigned long)event->START_COMPLETE.Status);
            ctx->server->reliable_send_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // Acknowledged (or canceled): the bytes are no longer queued/in flight.
        ctx->uncharge();
        break;

    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        // Viewer stopped reading this frame: abort our send side too.
        api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Final event: settle the accounting (a no-op unless SEND_COMPLETE was
        // never delivered), close the handle and free the context.
        ctx->uncharge();
        api->StreamClose(stream);
        delete ctx;
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

} // namespace parties::server
