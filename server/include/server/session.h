#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
#include <parties/video_backlog.h>

#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace parties::server {

enum class SessionPhase : uint8_t {
    Handshaking,
    Authenticated,
    SharingVideo,
    Retired,
};

class PublishedSessionPhase {
public:
    SessionPhase load() const { return phase_.load(std::memory_order_acquire); }
    bool authenticated() const {
        const SessionPhase p = load();
        return p == SessionPhase::Authenticated || p == SessionPhase::SharingVideo;
    }
    bool admits_video_streams() const { return load() == SessionPhase::SharingVideo; }

    void publish(SessionPhase p) { phase_.store(p, std::memory_order_release); }

private:
    std::atomic<SessionPhase> phase_{SessionPhase::Handshaking};
};

class SessionTransportStats {
public:
    int64_t video_outstanding_bytes() const {
        return video_outstanding_bytes_.load(std::memory_order_relaxed);
    }
    MinRtt min_rtt() const {
        return MinRtt::from_quic_min_rtt_us(min_rtt_us_.load(std::memory_order_relaxed));
    }
    bool alive() const { return alive_.load(std::memory_order_acquire); }

private:
    friend class QuicServer;
    friend class SessionTransport;

    std::atomic<int64_t>  video_outstanding_bytes_{0};
    std::atomic<uint32_t> min_rtt_us_{VIDEO_BACKLOG_DEFAULT_RTT_US};
    std::atomic<bool>     alive_{true};
};

class SessionTransport;

struct Session {
    explicit Session(uint32_t session_id);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const uint32_t id;

    PublishedSessionPhase phase;
    SessionTransportStats transport_stats;
    std::atomic<uint64_t> video_generation{0};
    std::atomic<int64_t>  video_pending_bytes{0};
    static constexpr int64_t VIDEO_INGRESS_MAX_BUFFERED_BYTES = 4 * 4 * 1024 * 1024;

    UserId           user_id = 0;
    std::string      username;
    int              role = 3;
    PublicKey         public_key{};
    uint16_t         protocol_version = protocol::PROTOCOL_VERSION_ASSUMED_LEGACY;
    bool supports_frame_streams() const {
        return protocol::protocol_supports_frame_streams(protocol_version);
    }

    std::unordered_map<UserId, ViewerVideoGate> video_gates;

    ChannelId        channel_id = 0;
    bool             muted = false;
    bool             deafened = false;

    uint8_t          share_codec = 0;
    uint16_t         share_width = 0;
    uint16_t         share_height = 0;

    std::unordered_set<UserId> subscribed_sharers;

private:
    friend class QuicServer;
    std::unique_ptr<SessionTransport> transport_;
};

}
