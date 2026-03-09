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
#include <client/stream_audio_capture.h>
#include <client/stream_audio_player.h>
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
class LevelMeterElement;
class LevelMeterInstancer;

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
    void poll_connecting();
    void finish_connect();
    void refresh_server_list();

    // Networking
    void process_server_messages();
    void send_auth_identity();
    void on_auth_response(const uint8_t* data, size_t len);
    void on_channel_list(const uint8_t* data, size_t len);
    void on_channel_user_list(const uint8_t* data, size_t len);
    void on_user_joined(const uint8_t* data, size_t len);
    void on_user_left(const uint8_t* data, size_t len);
    void on_user_role_changed(const uint8_t* data, size_t len);
    void on_server_error(const uint8_t* data, size_t len);
    void on_channel_key(const uint8_t* data, size_t len);
    void on_screen_share_started(const uint8_t* data, size_t len);
    void on_screen_share_stopped(const uint8_t* data, size_t len);
    void on_screen_share_denied(const uint8_t* data, size_t len);
    void on_admin_result(const uint8_t* data, size_t len);

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

    // Video encode thread (decouples capture from blocking GPU encode)
    void encode_loop();

    // Model + audio wiring
    void setup_model_callbacks();
    void setup_server_model_callbacks();
    void update_voice_level();
    void send_voice_state();
    void on_user_voice_state(const uint8_t* data, size_t len);

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
    bool awaiting_connection_ = false;  // True while waiting for async QUIC connect
    bool awaiting_channel_join_ = false;
    ChannelId pending_channel_id_ = 0;
    uint16_t voice_seq_ = 0;          // Sequence number for outgoing voice packets

    // PTT release delay
    std::chrono::steady_clock::time_point ptt_release_time_{};
    bool ptt_held_ = false;

    // Screen sharing state
    std::vector<CaptureTarget> capture_targets_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<VideoElementInstancer> video_instancer_;
    std::unique_ptr<LevelMeterInstancer> level_meter_instancer_;
    LevelMeterElement* level_meter_ = nullptr;  // owned by RmlUi document
    bool sharing_screen_ = false;
    std::atomic<bool> capture_lost_{false};  // set from WinRT thread when captured window closes
    uint32_t video_frame_number_ = 0;

    // Capture frame rate limiting (QPC-based)
    int64_t qpc_frequency_ = 0;
    int64_t capture_start_qpc_ = 0;
    int64_t last_capture_qpc_ = 0;
    int64_t capture_interval_qpc_ = 0;

    // Encode thread with triple-buffered staging textures.
    // Capture callback: CopyResource to free slot (fast GPU cmd), publish slot index.
    // Encode thread: pick latest slot, encode directly via NVENC registered input (zero-copy).
    static constexpr int ENCODE_SLOTS = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> encode_textures_[ENCODE_SLOTS];
    int encode_nvenc_slots_[ENCODE_SLOTS]{-1, -1, -1};  // NVENC registered input indices
    uint32_t encode_tex_w_ = 0, encode_tex_h_ = 0;
    bool encode_registered_ = false;  // true = zero-copy NVENC path active

    std::thread encode_thread_;
    std::atomic<bool> encode_running_{false};
    std::mutex encode_mutex_;
    std::condition_variable encode_cv_;

    // Triple-buffer slot management (protected by encode_mutex_)
    int encode_write_slot_ = 0;    // slot capture will write to next (pre-computed, always free)
    int encode_ready_slot_ = -1;   // slot with latest frame ready for encode (-1 = none)
    int encode_active_slot_ = -1;  // slot currently being encoded (-1 = idle)
    int64_t encode_ready_ts_ = 0;  // timestamp of ready frame

    uint32_t encode_fps_ = 60;
    std::function<void(const uint8_t*, size_t, bool)> encode_on_encoded_;

    // Stream audio (capture for sharer, playback for viewer)
    std::unique_ptr<StreamAudioCapture> stream_audio_capture_;
    StreamAudioPlayer stream_audio_player_;

    // Multi-sharer tracking
    struct SharerInfo {
        UserId user_id = 0;
        std::string name;
        VideoCodecId codec;
        uint16_t width = 0, height = 0;
    };
    std::unordered_map<UserId, SharerInfo> active_sharers_;
    UserId viewing_sharer_ = 0;
    bool awaiting_keyframe_ = false;  // skip non-keyframes until first keyframe after stream switch

    // Voice activity tracking (for speaking indicators)
    void update_speaking_state();
    std::unordered_map<UserId, std::chrono::steady_clock::time_point> voice_last_active_;

    // Video decode thread
    struct DecodeWork {
        std::vector<uint8_t> data;
        int64_t timestamp;
        VideoCodecId codec;
        uint16_t width;
        uint16_t height;
    };
    std::thread decode_thread_;
    std::atomic<bool> decode_running_{false};
    std::mutex decode_queue_mutex_;
    std::condition_variable decode_queue_cv_;
    std::queue<DecodeWork> decode_queue_;

    // Latest decoded frame — YUV planes passed directly to GPU (no CPU conversion)
    // Decode thread writes to staging_*, swaps with shared_* under lock.
    // Main thread swaps shared_* out. Buffers are reused across frames (no malloc after first).
    std::mutex frame_mutex_;
    std::atomic<bool> new_frame_available_{false};
    std::vector<uint8_t> shared_y_, shared_u_, shared_v_;
    std::vector<uint8_t> staging_y_, staging_u_, staging_v_;  // decode-thread-owned
    uint32_t shared_width_ = 0, shared_height_ = 0;
    uint32_t shared_y_stride_ = 0, shared_uv_stride_ = 0;
    bool shared_nv12_ = false;

    // FPS counters
    uint32_t fps_frame_count_ = 0;
    std::chrono::steady_clock::time_point fps_last_update_{std::chrono::steady_clock::now()};
    std::atomic<uint32_t> stream_frame_count_{0};  // incremented from encode/decode threads

    // Debounced preference saves (avoid SQLite writes on every slider tick)
    struct PendingPref {
        std::string value;
        std::chrono::steady_clock::time_point updated;
    };
    std::unordered_map<std::string, PendingPref> pending_prefs_;
    void save_pref_debounced(const std::string& key, std::string value);
    void flush_pending_prefs(bool force = false);
    void apply_user_audio_prefs(UserId user_id);

    // UI document
    Rml::ElementDocument* doc_ = nullptr;
};

} // namespace parties::client
