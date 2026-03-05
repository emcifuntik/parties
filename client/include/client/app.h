#pragma once

#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>
#include <client/ui_manager.h>
#include <client/settings.h>
#include <client/lobby_model.h>
#include <client/server_list_model.h>
#include <client/screen_capture.h>
#include <client/sound_player.h>
#include <parties/types.h>
#include <parties/video_common.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

class VideoEncoder;
class VideoDecoder;
class VideoElement;
class VideoElementInstancer;

class App {
public:
    App();
    ~App();

    bool init(HWND hwnd);
    void shutdown();

    // Main loop tick (called each frame)
    void update();

    // Public accessor for WndProc
    UiManager* ui_manager() { return &ui_; }

private:
    // Server list actions
    void do_connect();
    void refresh_server_list();

    // Networking
    void process_server_messages();
    void send_auth_identity();
    void on_auth_response(const uint8_t* data, size_t len);
    void on_channel_list(const uint8_t* data, size_t len);
    void on_channel_user_list(const uint8_t* data, size_t len);
    void on_user_joined(const uint8_t* data, size_t len);
    void on_user_left(const uint8_t* data, size_t len);
    void on_server_error(const uint8_t* data, size_t len);
    void on_channel_key(const uint8_t* data, size_t len);
    void on_screen_share_started(const uint8_t* data, size_t len);
    void on_screen_share_stopped(const uint8_t* data, size_t len);
    void on_screen_share_denied(const uint8_t* data, size_t len);

    void join_channel(ChannelId id);
    void leave_channel();

    // Screen sharing
    void show_share_picker();
    void start_screen_share(int target_index);
    void stop_screen_share();
    void on_video_frame_received(uint32_t sender_id, const uint8_t* data, size_t len);
    void watch_sharer(UserId id);
    void stop_watching();
    void send_pli(UserId target);
    void clear_all_sharers();
    void sync_sharer_model();

    // Video decode thread
    void start_decode_thread();
    void stop_decode_thread();
    void decode_loop();

    // Model + audio wiring
    void setup_model_callbacks();
    void setup_server_model_callbacks();
    void update_voice_level();
    void update_context_menu_position();

    HWND hwnd_ = nullptr;
    NetClient net_;
    AudioEngine audio_;
    VoiceMixer mixer_;
    SoundPlayer sound_player_;
    UiManager ui_;
    Settings settings_;
    LobbyModel model_;
    ServerListModel server_model_;

    // State
    bool authenticated_ = false;
    UserId user_id_ = 0;
    std::string username_;         // Display name
    int role_ = 3;
    std::string server_host_;
    uint16_t server_port_ = 7800;
    ChannelId current_channel_ = 0;
    ChannelKey channel_key_{};

    // Identity (loaded from settings on startup)
    SecretKey secret_key_{};
    PublicKey public_key_{};
    bool has_identity_ = false;

    int connecting_server_id_ = 0;

    // PTT release delay
    std::chrono::steady_clock::time_point ptt_release_time_{};
    bool ptt_held_ = false;

    // Screen sharing state
    std::vector<CaptureTarget> capture_targets_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<VideoElementInstancer> video_instancer_;
    bool sharing_screen_ = false;
    uint32_t video_frame_number_ = 0;

    // Multi-sharer tracking
    struct SharerInfo {
        UserId user_id = 0;
        std::string name;
        VideoCodecId codec;
        uint16_t width = 0, height = 0;
    };
    std::unordered_map<UserId, SharerInfo> active_sharers_;
    UserId viewing_sharer_ = 0;

    // Video decode thread
    struct DecodeWork {
        std::vector<uint8_t> data;
        int64_t timestamp;
    };
    std::thread decode_thread_;
    std::atomic<bool> decode_running_{false};
    std::mutex decode_queue_mutex_;
    std::condition_variable decode_queue_cv_;
    std::queue<DecodeWork> decode_queue_;

    // Latest decoded frame (decode thread → main thread)
    std::mutex frame_mutex_;
    std::atomic<bool> new_frame_available_{false};
    std::vector<uint8_t> shared_y_, shared_u_, shared_v_;
    uint32_t shared_width_ = 0, shared_height_ = 0;
    uint32_t shared_y_stride_ = 0, shared_uv_stride_ = 0;

    // BGRA conversion buffer for DComp video surface
    std::vector<uint8_t> bgra_buffer_;

    // Context menu positioning
    int context_menu_x_ = 0;
    int context_menu_y_ = 0;

    // UI document
    Rml::ElementDocument* doc_ = nullptr;
};

} // namespace parties::client
