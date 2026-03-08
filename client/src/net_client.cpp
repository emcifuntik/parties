#include <client/net_client.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>
#include <parties/protocol.h>
#include <parties/crypto.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace parties::client {

NetClient::NetClient() = default;

NetClient::~NetClient() {
    disconnect();
}

bool NetClient::connect(const std::string& host, uint16_t port,
                         const uint8_t* ticket, size_t ticket_len) {
	ZoneScopedN("NetClient::connect");
    if (connected_) return false;

    api_ = parties::quic_api();
    if (!api_) {
        std::fprintf(stderr, "[NetClient] MsQuic not initialized\n");
        return false;
    }

    QUIC_STATUS status;

    // Registration
    QUIC_REGISTRATION_CONFIG reg_config = { "parties_client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    status = api_->RegistrationOpen(&reg_config, &registration_);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[NetClient] RegistrationOpen failed: 0x%lx\n", (unsigned long)status);
        return false;
    }

    // Configuration with settings
    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = 60000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.KeepAliveIntervalMs = 15000;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    // Low-latency: disable send buffering (flush immediately) and pacing
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;
    settings.PacingEnabled = FALSE;
    settings.IsSet.PacingEnabled = TRUE;

    QUIC_BUFFER alpn = parties::make_alpn();
    status = api_->ConfigurationOpen(registration_, &alpn, 1, &settings,
                                      sizeof(settings), nullptr, &configuration_);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[NetClient] ConfigurationOpen failed: 0x%lx\n", (unsigned long)status);
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    // Client credentials — no certificate, disable validation (TOFU model)
    QUIC_CREDENTIAL_CONFIG cred_config = {};
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                        QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
                        QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                        QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;

    status = api_->ConfigurationLoadCredential(configuration_, &cred_config);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[NetClient] ConfigurationLoadCredential failed: 0x%lx\n", (unsigned long)status);
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    // Open connection
    status = api_->ConnectionOpen(registration_, connection_callback, this, &connection_);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[NetClient] ConnectionOpen failed: 0x%lx\n", (unsigned long)status);
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    // Apply resumption ticket for 0-RTT if available
    if (ticket && ticket_len > 0) {
        status = api_->SetParam(connection_, QUIC_PARAM_CONN_RESUMPTION_TICKET,
                                 static_cast<uint32_t>(ticket_len), ticket);
        if (QUIC_FAILED(status)) {
            std::fprintf(stderr, "[NetClient] SetParam(RESUMPTION_TICKET) failed: 0x%lx (non-fatal)\n", (unsigned long)status);
        }
    }

    // Start connection
    status = api_->ConnectionStart(connection_, configuration_,
                                    QUIC_ADDRESS_FAMILY_UNSPEC,
                                    host.c_str(), port);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[NetClient] ConnectionStart failed: 0x%lx\n", (unsigned long)status);
        api_->ConnectionClose(connection_);
        connection_ = nullptr;
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
        return false;
    }

    std::printf("[NetClient] Connecting to %s:%u via QUIC...\n", host.c_str(), port);

    // Connection is async — connected_ will be set in the CONNECTED event.
    // Poll is_connected() / connect_failed() from the UI thread.
    connecting_ = true;
    connect_failed_ = false;
    return true;
}

std::string NetClient::get_server_fingerprint() const {
    return server_fingerprint_;
}

void NetClient::disconnect() {
	ZoneScopedN("NetClient::disconnect");
    if (!connected_ && !connecting_ && !connection_) return;
    connected_ = false;
    connecting_ = false;
    connect_failed_ = false;

    if (connection_) {
        api_->ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        // Connection and stream cleanup happens in SHUTDOWN_COMPLETE callback
        // Wait briefly for clean shutdown
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force cleanup if callbacks haven't fired
    if (control_stream_) {
        control_stream_ = nullptr;
    }
    if (video_stream_) {
        video_stream_ = nullptr;
    }
    if (connection_) {
        api_->ConnectionClose(connection_);
        connection_ = nullptr;
    }
    if (configuration_) {
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }
    if (registration_) {
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        recv_buffer_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(video_buffer_mutex_);
        video_recv_buffer_.clear();
    }

    server_fingerprint_.clear();
}

bool NetClient::send_message(protocol::ControlMessageType type,
                              const uint8_t* payload, size_t payload_len) {
	ZoneScopedN("NetClient::send_message");
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!connected_ || !control_stream_) return false;

    // Wire format: [u32 length][u16 type][payload]
    uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
    size_t total_len = 6 + payload_len;

    auto* buf_data = new uint8_t[total_len];
    std::memcpy(buf_data, &msg_len, 4);
    uint16_t msg_type = static_cast<uint16_t>(type);
    std::memcpy(buf_data + 4, &msg_type, 2);
    if (payload_len > 0)
        std::memcpy(buf_data + 6, payload, payload_len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf_data };

    QUIC_STATUS status = api_->StreamSend(control_stream_, quic_buf, 1,
                                           QUIC_SEND_FLAG_NONE, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

bool NetClient::send_data(const uint8_t* data, size_t len, bool /*reliable*/) {
	ZoneScopedN("NetClient::send_data");
    if (!connected_ || !connection_) return false;

    // Both the data buffer AND the QUIC_BUFFER descriptor must be heap-allocated
    // because DatagramSend is async — MsQuic stores only the pointer.
    auto* buf_data = new uint8_t[len];
    std::memcpy(buf_data, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(len), buf_data };
    QUIC_STATUS status = api_->DatagramSend(connection_, quic_buf, 1,
                                             QUIC_SEND_FLAG_NONE, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

bool NetClient::send_video(const uint8_t* data, size_t len, bool /*reliable*/) {
	ZoneScopedN("NetClient::send_video");
    if (!connected_ || !video_stream_) return false;

    // Wire format: [u32 len][data]
    size_t total_len = 4 + len;
    auto* buf_data = new uint8_t[total_len];
    uint32_t frame_len = static_cast<uint32_t>(len);
    std::memcpy(buf_data, &frame_len, 4);
    std::memcpy(buf_data + 4, data, len);

    auto* quic_buf = new QUIC_BUFFER{ static_cast<uint32_t>(total_len), buf_data };
    QUIC_STATUS status = api_->StreamSend(video_stream_, quic_buf, 1,
                                           QUIC_SEND_FLAG_NONE, quic_buf);
    if (QUIC_FAILED(status)) {
        delete[] buf_data;
        delete quic_buf;
        return false;
    }
    return true;
}

// ── MsQuic callbacks ──

QUIC_STATUS QUIC_API NetClient::connection_callback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* client = static_cast<NetClient*>(context);
    return client->on_connection_event(connection, event);
}

QUIC_STATUS QUIC_API NetClient::stream_callback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* client = static_cast<NetClient*>(context);
    return client->on_stream_event(stream, event);
}

QUIC_STATUS NetClient::on_connection_event(HQUIC connection,
                                            QUIC_CONNECTION_EVENT* event) {
    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        std::printf("[NetClient] QUIC connected\n");
        connecting_ = false;

        // Open bidirectional control stream
        HQUIC stream = nullptr;
        QUIC_STATUS status = api_->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                               stream_callback, this, &stream);
        if (QUIC_SUCCEEDED(status)) {
            status = api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
            if (QUIC_SUCCEEDED(status)) {
                control_stream_ = stream;
            } else {
                std::fprintf(stderr, "[NetClient] Control StreamStart failed: 0x%lx\n", (unsigned long)status);
                api_->StreamClose(stream);
            }
        } else {
            std::fprintf(stderr, "[NetClient] Control StreamOpen failed: 0x%lx\n", (unsigned long)status);
        }

        // Open bidirectional video stream
        if (control_stream_) {
            HQUIC vstream = nullptr;
            status = api_->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                       stream_callback, this, &vstream);
            if (QUIC_SUCCEEDED(status)) {
                status = api_->StreamStart(vstream, QUIC_STREAM_START_FLAG_IMMEDIATE);
                if (QUIC_SUCCEEDED(status)) {
                    video_stream_ = vstream;
                    connected_ = true;
                } else {
                    std::fprintf(stderr, "[NetClient] Video StreamStart failed: 0x%lx\n", (unsigned long)status);
                    api_->StreamClose(vstream);
                    connected_ = true;  // Still connected, just no video stream
                }
            } else {
                std::fprintf(stderr, "[NetClient] Video StreamOpen failed: 0x%lx\n", (unsigned long)status);
                connected_ = true;  // Still connected
            }
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        // Voice/video data from server
        auto* buf = event->DATAGRAM_RECEIVED.Buffer;
        if (buf->Length > 0 && on_data_received)
            on_data_received(buf->Buffer, buf->Length);
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

    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
        // Extract server certificate fingerprint for TOFU verification.
        // With USE_PORTABLE_CERTIFICATES, Certificate is a QUIC_BUFFER* with DER bytes.
        auto* cert_buf = static_cast<QUIC_BUFFER*>(event->PEER_CERTIFICATE_RECEIVED.Certificate);
        if (cert_buf && cert_buf->Buffer && cert_buf->Length > 0) {
            server_fingerprint_ = parties::sha256_hex(
                cert_buf->Buffer, cert_buf->Length);
            std::printf("[NetClient] Server fingerprint: %s\n", server_fingerprint_.c_str());
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED: {
        auto& ticket_event = event->RESUMPTION_TICKET_RECEIVED;
        if (on_resumption_ticket && ticket_event.ResumptionTicketLength > 0) {
            on_resumption_ticket(ticket_event.ResumptionTicket,
                                 ticket_event.ResumptionTicketLength);
        }
        break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        std::printf("[NetClient] Connection shut down by transport: 0x%lx\n",
                    (unsigned long)event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        if (connecting_) connect_failed_ = true;
        connecting_ = false;
        connected_ = false;
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        std::printf("[NetClient] Connection shut down by server\n");
        if (connecting_) connect_failed_ = true;
        connecting_ = false;
        connected_ = false;
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        std::printf("[NetClient] Connection shutdown complete\n");
        if (connecting_) connect_failed_ = true;
        connecting_ = false;
        connected_ = false;
        control_stream_ = nullptr;
        video_stream_ = nullptr;

        if (on_disconnected)
            on_disconnected();
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS NetClient::on_stream_event(HQUIC stream, QUIC_STREAM_EVENT* event) {
    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Route to control or video stream processor
        bool is_video = (stream == video_stream_);
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; i++) {
            auto& buf = event->RECEIVE.Buffers[i];
            if (is_video)
                process_video_stream_data(buf.Buffer, buf.Length);
            else
                process_stream_data(buf.Buffer, buf.Length);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* quic_buf = static_cast<QUIC_BUFFER*>(event->SEND_COMPLETE.ClientContext);
        if (quic_buf) {
            delete[] quic_buf->Buffer;
            delete quic_buf;
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        api_->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (stream == control_stream_)
            control_stream_ = nullptr;
        else if (stream == video_stream_)
            video_stream_ = nullptr;
        api_->StreamClose(stream);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

void NetClient::process_stream_data(const uint8_t* data, size_t len) {
	ZoneScopedN("NetClient::process_stream_data");
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    recv_buffer_.insert(recv_buffer_.end(), data, data + len);

    // Parse complete messages: [u32 length][u16 type][payload]
    while (recv_buffer_.size() >= 6) {
        uint32_t msg_len;
        std::memcpy(&msg_len, recv_buffer_.data(), 4);

        if (msg_len < 2 || msg_len > 1024 * 1024) {
            std::fprintf(stderr, "[NetClient] Invalid message length %u\n", msg_len);
            recv_buffer_.clear();
            break;
        }

        size_t total_needed = 4 + msg_len;
        if (recv_buffer_.size() < total_needed) break;

        uint16_t raw_type;
        std::memcpy(&raw_type, recv_buffer_.data() + 4, 2);

        uint32_t payload_len = msg_len - 2;
        ServerMessage msg;
        msg.type = static_cast<protocol::ControlMessageType>(raw_type);
        if (payload_len > 0)
            msg.payload.assign(recv_buffer_.data() + 6, recv_buffer_.data() + 6 + payload_len);

        incoming_.push(std::move(msg));

        recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + total_needed);
    }
}

void NetClient::process_video_stream_data(const uint8_t* data, size_t len) {
	ZoneScopedN("NetClient::process_video_stream_data");
    std::lock_guard<std::mutex> lock(video_buffer_mutex_);
    video_recv_buffer_.insert(video_recv_buffer_.end(), data, data + len);

    // Parse complete frames: [u32 length][data]
    while (video_recv_buffer_.size() >= 4) {
        uint32_t frame_len;
        std::memcpy(&frame_len, video_recv_buffer_.data(), 4);

        if (frame_len == 0 || frame_len > 4 * 1024 * 1024) {
            std::fprintf(stderr, "[NetClient] Invalid video frame length %u\n", frame_len);
            video_recv_buffer_.clear();
            break;
        }

        size_t total_needed = 4 + frame_len;
        if (video_recv_buffer_.size() < total_needed) break;

        // Dispatch the raw frame data (same format as datagrams)
        if (on_data_received)
            on_data_received(video_recv_buffer_.data() + 4, frame_len);

        video_recv_buffer_.erase(video_recv_buffer_.begin(),
                                  video_recv_buffer_.begin() + total_needed);
    }
}

} // namespace parties::client
