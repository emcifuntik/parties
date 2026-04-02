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
    void handle_message(const IncomingMessage& msg);
    void on_client_disconnect(uint32_t session_id);

    void send_error(uint32_t session_id, const std::string& message);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);
    void send_channel_key(uint32_t session_id, ChannelId channel_id);

    // Screen sharing
    void forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void forward_stream_audio(const DataPacket& pkt);
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);

    Config config_;
    Database db_;
    QuicServer quic_;
    std::atomic<bool> running_{false};
    std::unordered_map<ChannelId, std::array<uint8_t, 32>> channel_keys_;

    // Screen share state: channel_id -> set of sharer user_ids
    std::mutex sharers_mutex_;
    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;
};

} // namespace parties::server
