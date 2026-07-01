#pragma once

#include <server/config.h>
#include <server/quic_server.h>
#include <server/database.h>
#include <parties/types.h>
#include <parties/video_common.h>

#include <array>
#include <atomic>
#include <set>
#include <thread>
#include <unordered_map>

namespace parties::server {

class Server {
public:
    Server();
    ~Server();

    // Initialize with config, start listening
    bool start(const Config& cfg);

    // Run one iteration of the main loop
    void run();

    // Signal the server to stop
    void stop();

private:
    void process_control_messages();
    void process_data_packets();
    void process_file_transfers();
    void process_disconnects();
    void handle_message(const IncomingMessage& msg);
    // Broadcast USER_LEFT and clean up screen-share state for a session that
    // dropped. Runs on the main loop only (see SessionDisconnect queue).
    void process_disconnect(uint32_t session_id, UserId user_id, ChannelId channel_id);

    void send_error(uint32_t session_id, const std::string& message,
                    protocol::ServerErrorCode code = protocol::ServerErrorCode::Generic);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);

    // Screen sharing
    void forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void forward_stream_audio(const DataPacket& pkt);
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);

    // Webcam sharing (independent parallel stream, mirrors screen sharing)
    void forward_camera_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void stop_camera_share(ChannelId channel_id, UserId user_id);

    Config config_;
    Database db_;
    QuicServer quic_;
    std::atomic<bool> running_{false};

    // Screen share state: channel_id -> set of sharer user_ids
    std::mutex sharers_mutex_;
    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;

    // Webcam share state: channel_id -> set of camera-streaming user_ids.
    // Guarded by sharers_mutex_ (same as channel_screen_sharers_).
    std::unordered_map<ChannelId, std::set<UserId>> channel_camera_sharers_;

    // Guards every access to Session::subscribed_sharers AND
    // subscribed_camera_sharers. forward_video_frame / forward_camera_frame read
    // them on the MsQuic receive thread while the *_SHARE_VIEW / channel leave /
    // stop_*_share paths mutate them on the main loop: an unordered_set read
    // concurrent with insert/erase/clear is UB without this.
    std::mutex subscriptions_mutex_;

    // Auth replay guard: recently-accepted (pubkey, timestamp) pairs. Drops an
    // AUTH_IDENTITY whose signed blob we've already seen inside the freshness
    // window, defeating replay of a captured handshake. Pruned by timestamp.
    std::unordered_map<Fingerprint, uint64_t> recent_auth_;
};

} // namespace parties::server
