#pragma once

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
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace parties::server {

class Server {
public:
    Server();
    ~Server();

    bool start(const Config& cfg);

    void run();

    void stop();

private:
    void process_control_messages();
    void process_data_packets();
    void enqueue_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void process_video_frames();
    void process_file_transfers();
    void process_disconnects();
    void handle_message(const IncomingMessage& msg);
    void process_disconnect(uint32_t session_id, UserId user_id, ChannelId channel_id);

    void send_error(uint32_t session_id, const std::string& message,
                    protocol::ServerErrorCode code = protocol::ServerErrorCode::Generic);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);

    void forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void forward_stream_audio(const DataPacket& pkt);
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);
    void request_keyframe_from_sharer(UserId sharer_user_id, UserId requester);
    void notify_viewer_change(UserId sharer_user_id, UserId viewer_user_id, bool watching);
    std::shared_ptr<Session> session_for_user(UserId user_id);
    void clear_viewer_subscriptions(const std::shared_ptr<Session>& session, bool notify);

    Config config_;
    Database db_;
    QuicServer quic_;
    std::atomic<bool> running_{false};

    struct PendingVideoFrame {
        std::shared_ptr<Session> session;
        uint64_t generation;
        std::vector<uint8_t> data;
    };
    ThreadQueue<PendingVideoFrame> video_incoming_;
    struct VideoIngress {
        std::shared_ptr<Session> session;
        VideoFrameReorderBuffer reorder;
    };
    std::unordered_map<uint32_t, VideoIngress> video_ingress_;

    std::mutex sharers_mutex_;
    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;

    std::mutex subscriptions_mutex_;

    struct SharerVideoState {
        RateEstimator rate{1.0};
        Coalescer     pli{200'000};
        uint32_t      last_frame_seq = 0;
        bool          have_frame = false;
        Coalescer     drop_log{1'000'000};
        uint32_t      drops_since_log = 0;
        int64_t       max_keyframe_bytes = 0;
        int64_t       max_keyframe_at_us = 0;
    };
    std::mutex video_state_mutex_;
    std::unordered_map<UserId, SharerVideoState> sharer_video_state_;

    int64_t viewer_backlog_threshold(const Session& viewer,
                                     const std::unordered_map<UserId, double>& sharer_rates,
                                     int64_t keyframe_headroom) const;


    std::unordered_map<uint32_t, TokenBucket> video_control_buckets_;

    std::mutex user_index_mutex_;
    std::unordered_map<UserId, uint32_t> user_sessions_;

    std::unordered_map<uint32_t, std::shared_ptr<Session>> authed_sessions_;

    std::unordered_map<Fingerprint, uint64_t> recent_auth_;
};

}
