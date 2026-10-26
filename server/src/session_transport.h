#pragma once

#include <server/session.h>
#include <parties/quic_common.h>
#include <parties/video_backlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace parties::server {

class SessionTransport {
public:
    SessionTransport(HQUIC connection, SessionTransportStats& stats)
        : connection_(connection), stats_(stats) {}

    enum class PeerStreamRole { Control, LegacyVideo, FileTransfer };

    PeerStreamRole adopt_peer_bidi_stream(HQUIC stream) {
        std::lock_guard lock(m_);
        if (!control_) {
            control_ = stream;
            return PeerStreamRole::Control;
        }
        if (!legacy_video_) {
            legacy_video_ = stream;
            return PeerStreamRole::LegacyVideo;
        }
        return PeerStreamRole::FileTransfer;
    }

    bool is_legacy_video_stream(HQUIC stream) {
        std::lock_guard lock(m_);
        return stream == legacy_video_;
    }

    void forget_stream(HQUIC stream) {
        std::lock_guard lock(m_);
        if (control_ == stream)
            control_ = nullptr;
        else if (legacy_video_ == stream)
            legacy_video_ = nullptr;
    }

    enum class Stream { Control, LegacyVideo };

    std::optional<QUIC_STATUS> send_on(const QUIC_API_TABLE& api, Stream which,
                                       QUIC_BUFFER* bufs, uint32_t count, void* ctx) {
        std::lock_guard lock(m_);
        HQUIC stream = which == Stream::Control ? control_ : legacy_video_;
        if (!open() || !stream) return std::nullopt;
        return api.StreamSend(stream, bufs, count, QUIC_SEND_FLAG_NONE, ctx);
    }

    std::optional<QUIC_STATUS> send_datagram(const QUIC_API_TABLE& api, QUIC_BUFFER* buf,
                                             QUIC_SEND_FLAGS flags, void* ctx) {
        std::lock_guard lock(m_);
        if (!open() || !connection_) return std::nullopt;
        return api.DatagramSend(connection_, buf, 1, flags, ctx);
    }

    enum class FrameStreamResult { NotConnected, OpenFailed, SendFailed, Sent };

    FrameStreamResult send_on_new_frame_stream(const QUIC_API_TABLE& api,
                                               QUIC_STREAM_CALLBACK_HANDLER handler, void* ctx,
                                               QUIC_BUFFER* bufs, uint32_t count,
                                               std::atomic<uint32_t>& charged) {
        std::lock_guard lock(m_);
        if (!open() || !connection_) return FrameStreamResult::NotConnected;

        HQUIC stream = nullptr;
        if (QUIC_FAILED(api.StreamOpen(connection_, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                       handler, ctx, &stream)))
            return FrameStreamResult::OpenFailed;

        uint32_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += bufs[i].Length;
        stats_.video_outstanding_bytes_.fetch_add(total, std::memory_order_relaxed);
        charged.store(total, std::memory_order_release);

        if (QUIC_FAILED(api.StreamSend(stream, bufs, count,
                                       QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, ctx))) {
            if (const uint32_t n = charged.exchange(0, std::memory_order_acq_rel))
                stats_.video_outstanding_bytes_.fetch_sub(n, std::memory_order_relaxed);
            api.StreamShutdown(stream,
                QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE, 0);
            return FrameStreamResult::SendFailed;
        }
        return FrameStreamResult::Sent;
    }

    void shutdown_connection(const QUIC_API_TABLE& api) {
        std::lock_guard lock(m_);
        stats_.alive_.store(false, std::memory_order_release);
        if (connection_)
            api.ConnectionShutdown(connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }

    void retire_handles() {
        std::lock_guard lock(m_);
        stats_.alive_.store(false, std::memory_order_release);
        connection_   = nullptr;
        control_      = nullptr;
        legacy_video_ = nullptr;
    }

    void sample_min_rtt_if_due(const QUIC_API_TABLE& api, HQUIC stream, int64_t now_us) {
        if (rtt_sample_last_us_ && now_us - *rtt_sample_last_us_ < 1'000'000) return;
        rtt_sample_last_us_ = now_us;
        QUIC_STATISTICS_V2 stats{};
        uint32_t size = sizeof(stats);
        if (QUIC_SUCCEEDED(api.GetParam(stream, QUIC_PARAM_CONN_STATISTICS_V2, &size, &stats))) {
            stats_.min_rtt_us_.store(std::clamp<uint32_t>(stats.MinRtt, 1'000, VIDEO_BACKLOG_MAX_RTT_US),
                                     std::memory_order_relaxed);
        }
    }

    int64_t charge_ingress(int64_t bytes) {
        return ingress_buffered_bytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    }
    void uncharge_ingress(int64_t bytes) {
        ingress_buffered_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }

private:
    bool open() const { return stats_.alive_.load(std::memory_order_relaxed); }

    std::mutex m_;
    HQUIC connection_   = nullptr;
    HQUIC control_      = nullptr;
    HQUIC legacy_video_ = nullptr;
    SessionTransportStats& stats_;

    std::optional<int64_t> rtt_sample_last_us_;
    std::atomic<int64_t>   ingress_buffered_bytes_{0};
};

}
