#pragma once

#include <server/config.h>
#include <server/quic_server.h>
#include <server/database.h>
#include <server/plugin_manager.h>
#include <parties/types.h>
#include <parties/video_common.h>

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <set>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <optional>
#include <string_view>
#include <utility>

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
    void process_plugin_host_calls();
    void handle_message(const IncomingMessage& msg);
    // Broadcast USER_LEFT and clean up screen-share state for a session that
    // dropped. Runs on the main loop only (see SessionDisconnect queue).
    void process_disconnect(uint32_t session_id, UserId user_id, ChannelId channel_id);

    void send_error(uint32_t session_id, const std::string& message,
                    protocol::ServerErrorCode code = protocol::ServerErrorCode::Generic);
    void send_channel_list(uint32_t session_id);
    void send_text_channel_list(uint32_t session_id);
    void send_chat_command_list(uint32_t session_id);
    std::optional<UserId> create_plugin_bot_user(std::string_view plugin_id,
                                                 std::string_view key,
                                                 std::string_view display_name);
    std::optional<uint64_t> store_and_broadcast_chat_message(UserId sender_id,
                                                             std::string_view sender_name,
                                                             uint32_t channel_id,
                                                             std::string_view text);
    bool join_plugin_bot_voice(UserId user_id,
                               std::string_view display_name,
                               ChannelId voice_channel_id);
    bool leave_plugin_bot_voice(UserId user_id,
                                std::string_view display_name,
                                ChannelId voice_channel_id);
    bool send_plugin_bot_voice_packet(UserId user_id,
                                      ChannelId voice_channel_id,
                                      uint16_t sequence,
                                      const uint8_t* opus_payload,
                                      size_t opus_payload_len);

    bool is_server_thread() const {
        return server_thread_id_ == std::this_thread::get_id();
    }

    template <typename Fn>
    auto invoke_on_server_thread(Fn&& fn) -> std::invoke_result_t<Fn> {
        using Result = std::invoke_result_t<Fn>;
        if (is_server_thread())
            return std::forward<Fn>(fn)();

        if (!running_.load(std::memory_order_acquire)) {
            if constexpr (std::is_void_v<Result>) {
                return;
            } else {
                return Result{};
            }
        }

        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        auto future = task->get_future();
        plugin_host_calls_.push([task]() { (*task)(); });
        if constexpr (std::is_void_v<Result>) {
            future.get();
        } else {
            return future.get();
        }
    }

    // Screen sharing
    void forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len);
    void forward_stream_audio(const DataPacket& pkt);
    void handle_video_control(const DataPacket& pkt);
    void stop_screen_share(ChannelId channel_id, UserId user_id);

    Config config_;
    Database db_;
    QuicServer quic_;
    PluginManager plugins_;
    std::atomic<bool> running_{false};
    std::thread::id server_thread_id_;
    ThreadQueue<std::function<void()>> plugin_host_calls_;

    // Screen share state: channel_id -> set of sharer user_ids
    std::mutex sharers_mutex_;
    std::unordered_map<ChannelId, std::set<UserId>> channel_screen_sharers_;

    // Auth replay guard: recently-accepted (pubkey, timestamp) pairs. Drops an
    // AUTH_IDENTITY whose signed blob we've already seen inside the freshness
    // window, defeating replay of a captured handshake. Pruned by timestamp.
    std::unordered_map<Fingerprint, uint64_t> recent_auth_;
};

} // namespace parties::server
