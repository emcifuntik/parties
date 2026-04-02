#pragma once

#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>
#include <client/settings.h>
#include <client/lobby_model.h>
#include <client/chat_model.h>
#include <client/server_list_model.h>
#include <client/sound_player.h>
#include <client/stream_audio_player.h>
#include <parties/types.h>
#include <parties/video_common.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Rml { class Context; }

namespace parties::client {

struct PlatformBridge {
    std::function<void(const std::string&)>                    copy_to_clipboard;
    std::function<void(SoundPlayer::Effect)>                   play_sound;
    std::function<void(float)>                                 set_notification_volume;
    std::function<void(int channel_id, const std::string& name)> show_channel_menu;
    std::function<void(int server_id)>                         show_server_menu;
    std::function<void(int64_t message_id)>                    show_message_menu;
    std::function<void()>                                      open_share_picker;
    std::function<void()>                                      on_authenticated;
    std::function<void()>                                      stop_screen_share;
    std::function<void()>                                      request_keyframe;
    std::function<void()>                                      clear_video_element;
    std::function<void()>                                      start_decode_thread;
    std::function<void()>                                      stop_decode_thread;
};

class AppCore {
public:
    AppCore();
    ~AppCore();

    bool init(const std::string& settings_path, PlatformBridge bridge, Rml::Context* rml_context);
    void shutdown();
    void tick();
    void load_saved_prefs();

    // Platform-provided video frame handler (platform owns decoder)
    std::function<void(uint32_t sender_id, const uint8_t* data, size_t len)> on_video_frame_received;

    // Public subsystems
    NetClient         net_;
    AudioEngine       audio_;
    VoiceMixer        mixer_;
    Settings          settings_;
    LobbyModel        model_;
    ChatModel         chat_model_;
    ServerListModel   server_model_;
    StreamAudioPlayer stream_audio_player_;

    // Shared state
    bool        authenticated_        = false;
    UserId      user_id_              = 0;
    std::string username_;
    int         role_                 = 3;
    std::string server_host_;
    uint16_t    server_port_          = 7800;
    std::string server_password_;
    int         connecting_server_id_ = 0;
    ChannelId   current_channel_      = 0;
    ChannelKey  channel_key_{};
    SecretKey   secret_key_{};
    PublicKey   public_key_{};
    bool        has_identity_         = false;
    std::string seed_phrase_;
    bool        awaiting_connection_   = false;
    bool        awaiting_channel_join_ = false;
    ChannelId   pending_channel_id_    = 0;
    uint16_t    voice_seq_             = 0;
    UserId      viewing_sharer_        = 0;
    bool        awaiting_keyframe_     = false;
    uint32_t    video_frame_number_    = 0;
    std::atomic<uint32_t> stream_frame_count_{0};
    std::string tofu_pending_fingerprint_;
    bool        tofu_pending_ = false;

    // Actions
    void watch_sharer(UserId id);
    void stop_watching();
    void send_voice_state();
    void send_pli(UserId target);
    void clear_all_sharers();
    void refresh_server_list();
    void apply_user_audio_prefs(UserId user_id);
    void save_pref_debounced(const std::string& key, std::string value);
    void flush_pending_prefs(bool force = false);
    void load_or_generate_identity(const std::string& username_hint = "");

    // Called each frame — processes network messages from net_.incoming()
    void process_server_messages();
    void handle_server_message(protocol::ControlMessageType type, const uint8_t* data, size_t len);

    // Connection flow
    void do_connect();
    void poll_connecting();
    void finish_connect();
    void send_auth_identity();
    void on_disconnect_cleanup();

    // Model callbacks (called from init())
    void setup_model_callbacks();
    void setup_chat_model_callbacks();
    void setup_server_model_callbacks();

    // Channel operations (public so platform can call join_channel if needed)
    void join_channel(ChannelId id);
    void leave_channel();

private:
    PlatformBridge bridge_;

    struct SharerInfo { UserId user_id = 0; std::string name; };
    std::unordered_map<UserId, SharerInfo> active_sharers_;

    std::unordered_map<UserId, std::chrono::steady_clock::time_point> voice_last_active_;

    struct PendingPref {
        std::string value;
        std::chrono::steady_clock::time_point updated;
    };
    std::unordered_map<std::string, PendingPref> pending_prefs_;

    std::chrono::steady_clock::time_point stream_fps_last_update_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point ping_sent_at_{};
    std::chrono::steady_clock::time_point ping_last_send_{};
    bool ping_pending_ = false;

#ifdef _WIN32
    std::unique_ptr<class AutoUpdater> updater_;
#endif

    // Pending file uploads: file data cached until CHAT_MESSAGE assigns attachment IDs
    struct PendingUpload {
        std::string path;
        std::vector<uint8_t> data;
    };
    std::vector<PendingUpload> pending_uploads_;

    // Pending file downloads: received from QUIC thread, processed on main thread
    struct CompletedDownload {
        uint64_t attachment_id;
        std::vector<uint8_t> data;
    };
    std::mutex downloads_mutex_;
    std::vector<CompletedDownload> completed_downloads_;

    void on_auth_response(const uint8_t* data, size_t len);
    void on_channel_list(const uint8_t* data, size_t len);
    void on_channel_user_list(const uint8_t* data, size_t len);
    void on_user_joined(const uint8_t* data, size_t len);
    void on_user_left(const uint8_t* data, size_t len);
    void on_user_voice_state(const uint8_t* data, size_t len);
    void on_user_role_changed(const uint8_t* data, size_t len);
    void on_channel_key(const uint8_t* data, size_t len);
    void on_screen_share_started(const uint8_t* data, size_t len);
    void on_screen_share_stopped(const uint8_t* data, size_t len);
    void on_screen_share_denied(const uint8_t* data, size_t len);
    void on_admin_result(const uint8_t* data, size_t len);
    void on_server_error(const uint8_t* data, size_t len);
    void on_chat_channel_list(const uint8_t* data, size_t len);
    void on_chat_message(const uint8_t* data, size_t len);
    void on_chat_history_resp(const uint8_t* data, size_t len);
    void on_chat_message_deleted(const uint8_t* data, size_t len);
    void on_chat_search_resp(const uint8_t* data, size_t len);
    void on_chat_pinned_resp(const uint8_t* data, size_t len);
    void on_chat_file_ready(const uint8_t* data, size_t len);

    void update_speaking_state();
    void generate_identity();
};

} // namespace parties::client
