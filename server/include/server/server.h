#pragma once

#include <server/auth_replay_guard.h>
#include <server/config.h>
#include <server/quic_server.h>
#include <server/database.h>
#include <parties/types.h>
#include <parties/video_common.h>
#include <parties/video_backlog.h>
#include <parties/video_frame_reorder.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace parties::server {

class Server {
public:
    Server();
    ~Server();

    bool start(const Config& cfg);

    void run_until_stopped();

    void request_stop();

private:
    void process_control_messages();
    void process_data_packets();
    void process_video_frames();
    void process_file_transfers();
    void process_disconnects();
    void handle_message(const IncomingMessage& msg);
    void process_disconnect(const std::shared_ptr<Session>& session);

    void send_error(uint32_t session_id, const std::string& message,
                    protocol::ServerErrorCode code = protocol::ServerErrorCode::Generic);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);

    void forward_video_frame(const Session& sharer, const ParsedVideoFrame& frame);
    void forward_stream_audio(const DataPacket& pkt);
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);
    void request_keyframe_from_sharer(UserId sharer_user_id, UserId requester);
    void notify_viewer_change(UserId sharer_user_id, UserId viewer_user_id, bool watching);
    std::shared_ptr<Session> session_for_user(UserId user_id);
    bool is_screen_sharer(ChannelId channel_id, UserId user_id) const;
    void clear_viewer_subscriptions(const std::shared_ptr<Session>& session, bool notify);

    Config config_;
    Database db_;
    QuicServer quic_;
    AuthReplayGuard auth_replay_;
    std::atomic<bool> stop_requested_{false};
    static_assert(std::atomic<bool>::is_always_lock_free);

    struct VideoIngress {
        std::shared_ptr<Session> session;
        VideoFrameReorderBuffer reorder;
    };
    std::unordered_map<uint32_t, VideoIngress> video_ingress_;

    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;

    struct SharerVideoState {
        RateEstimator rate{1.0};
        Coalescer     pli{200'000};
        Coalescer     drop_log{1'000'000};
        uint32_t      drops_since_log = 0;
        int64_t       max_keyframe_bytes = 0;
        std::optional<int64_t> max_keyframe_at_us;
    };
    std::unordered_map<UserId, SharerVideoState> sharer_video_state_;

    int64_t viewer_backlog_threshold(const Session& viewer,
                                     const std::unordered_map<UserId, double>& sharer_rates,
                                     int64_t keyframe_headroom) const;


    std::unordered_map<uint32_t, TokenBucket> video_control_buckets_;

    std::unordered_map<UserId, uint32_t> user_sessions_;
};

}
