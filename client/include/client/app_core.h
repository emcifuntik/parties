#pragma once

#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>
#include <client/settings.h>
#include <client/lobby_model.h>
#include <client/chat_model.h>
#include <client/idle_sleep_inhibitor.h>
#include <client/server_list_model.h>
#include <client/server_query_wake.h>
#include <client/sound_player.h>
#include <client/stream_audio_player.h>
#include <client/stream_pip.h>
#include <parties/types.h>
#include <parties/video_common.h>
#include <parties/video_frame_reorder.h>
#include <parties/video_send_controller.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Rml { class Context; }

namespace parties::client {

struct UserContextWindowRequest {
    int user_id = 0;
    std::string name;
    std::string channel_name;
    int role = 3;
    bool can_manage_roles = false;
    bool can_kick = false;
    float volume = 1.0f;
    float music_volume = 1.0f;
    bool compression = false;
    float compression_target = 0.8f;
};

struct MultiStreamVideo {
    std::function<void(UserId)> start_stream;
    std::function<void(UserId)> stop_stream;
    std::function<bool(UserId)> wants_pli_retry;
};

struct SingleDecoderVideo {};

using VideoPresentation = std::variant<MultiStreamVideo, SingleDecoderVideo>;

struct PictureInPicture {
    std::function<void(UserId, const std::string& title)> show;
    std::function<void(UserId, PipCloseReason)>           hide;
};

struct PickerShareFlow {
    std::function<void()> open_picker;
    std::function<void()> open_audio_picker;
    std::function<void()> cancel_picker;
};

struct NativeShareFlow {
    std::function<void()> start_native_share;
    std::function<void()> start_native_audio_share;
};

struct NoShareFlow {};

using ShareFlow = std::variant<PickerShareFlow, NativeShareFlow, NoShareFlow>;

struct PlatformBridge {
    std::function<void(const std::string&)>                    copy_to_clipboard;
    std::function<void(SoundPlayer::Effect)>                   play_sound;
    std::function<void(float)>                                 set_notification_volume;
    std::function<void(const UserContextWindowRequest&)>       show_user_menu;
    std::function<void(int channel_id, const std::string& name)> show_channel_menu;
    std::function<void(int server_id)>                         show_server_menu;
    std::function<void(int64_t message_id)>                    show_message_menu;
    std::function<void()>                                      stop_screen_share;
    std::function<void()>                                      stop_audio_share;
    std::function<void()>                                      request_keyframe;
    std::function<void()>                                      clear_video_element;
    std::function<void(bool)>                                  set_keep_awake;
    std::function<void()>                                      show_main_window;
    std::function<void()>                                      on_audio_devices_published;
    VideoPresentation                                          video;
    ShareFlow                                                  share;
    std::optional<PictureInPicture>                            pip;
};

// on_frame/on_aborted run only on the connection's MsQuic worker, so frames of
// one sharer stay ordered after the lock is released for delivery.
class ReorderBuffers {
public:
    [[nodiscard]] ReorderOutcome on_frame(UserId sharer, ParsedVideoFrame&& frame, int64_t now_us);
    [[nodiscard]] KeyframeRequest on_aborted(UserId sharer, uint32_t frame_seq);
    [[nodiscard]] std::vector<UserId> poll(int64_t now_us);
    bool wants_pli_retry(UserId sharer) const;
    void erase(UserId sharer);
    void clear();

private:
    mutable std::mutex m_;
    std::unordered_map<UserId, VideoFrameReorderBuffer> buffers_;
};

class PliLimiter {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] bool admit_outgoing(UserId target, Clock::time_point now);
    [[nodiscard]] bool admit_incoming(Clock::time_point now);
    void clear();

private:
    std::mutex m_;
    std::unordered_map<UserId, Clock::time_point> last_sent_;
    std::optional<Clock::time_point> last_incoming_;
};

enum class SingleWatchAdmit { Decode, DropUntilKeyframe, NotWatched };

class AppCore {
public:
    AppCore();
    ~AppCore();

    bool init(const std::string& settings_path, PlatformBridge bridge, Rml::Context* rml_context);
    void shutdown();
    void tick();
    void load_saved_prefs();

    std::function<void(UserId sender, ParsedVideoFrame&& frame)> on_video_frame_received;

private:
    VideoSendController video_sender_;

public:
    NetClient         net_;
    AudioEngine       audio_;
    VoiceMixer        mixer_;
    VoiceMixer        aux_mixer_{false};
    Settings          settings_;
    LobbyModel        model_;
    ChatModel         chat_model_;
    ServerListModel   server_model_;
    StreamAudioPlayer stream_audio_player_;

    bool        authenticated_        = false;
    UserId      user_id_              = 0;
    std::string global_name_;
    std::string username_;
    int         role_                 = 3;
    std::string server_host_;
    uint16_t    server_port_          = 7800;
    std::string server_password_;
    int         connecting_server_id_ = 0;
    ChannelId   current_channel_      = 0;
    SecretKey   secret_key_{};
    PublicKey   public_key_{};
    bool        has_identity_         = false;
    std::string seed_phrase_;
    bool        awaiting_connection_   = false;
    bool        awaiting_channel_join_ = false;
    ChannelId   pending_channel_id_    = 0;
    uint16_t    voice_seq_             = 0;
    uint16_t    voice2_seq_            = 0;
    std::atomic<uint32_t> video_frame_number_{0};
    std::atomic<uint32_t> stream_frame_count_{0};
    uint16_t    server_protocol_version_ = protocol::PROTOCOL_VERSION_ASSUMED_LEGACY;

    void begin_video_share(uint32_t ceiling_bitrate_bps);
    void set_video_ceiling_bitrate(uint32_t bps);
    VideoSendAccounting& video_accounting() { return video_sender_.accounting(); }
    VideoEncodeFeedback& video_encode_feedback() { return video_sender_.encode_feedback(); }

    struct VideoSendResult {
        uint32_t frame_seq = 0;
        bool     sent = false;
    };
    VideoSendResult send_video_frame(const uint8_t* encoded, size_t len, bool keyframe,
                                     uint16_t width, uint16_t height, VideoCodecId codec);
    std::string tofu_pending_fingerprint_;
    bool        tofu_pending_ = false;
    std::atomic<bool> disconnect_pending_{false};

    std::mutex           pending_ticket_mutex_;
    std::vector<uint8_t> pending_resumption_ticket_;
    bool                 pending_ticket_dirty_ = false;

    bool        intentional_disconnect_ = false;
    bool        reconnecting_           = false;
    int         reconnect_attempts_     = 0;
    int         reconnect_flaps_        = 0;
    ChannelId   reconnect_channel_      = 0;
    bool        rejoin_pending_         = false;
    std::chrono::steady_clock::time_point reconnect_next_attempt_{};
    std::chrono::steady_clock::time_point auth_success_time_{};

    void watch_sharer(UserId id);
    void stop_watching();
    void add_watch(UserId id);
    void remove_watch(UserId id);
    void toggle_watch(UserId id);
    bool is_watching(UserId id) const;
    void set_single_watched(UserId id);
    UserId viewing_sharer() const { return viewing_sharer_.load(); }

    void begin_single_watch(UserId sharer);
    void end_single_watch();
    [[nodiscard]] SingleWatchAdmit admit_single_watch_frame(UserId sender, const VideoFrameHeader& header);
    void send_voice_state();

    StreamPipController pip_;
    void open_pip(UserId id);
    void toggle_pip(UserId id);
    void close_pip(PipCloseReason reason = PipCloseReason::UserClosed);
    void return_from_pip();
    void set_stream_volume(float volume);
    void send_pli(UserId target);
    void clear_all_sharers();
    void refresh_server_list();
    void apply_user_audio_prefs(UserId user_id);
    void save_pref_debounced(const std::string& key, std::string value);
    void flush_pending_prefs(bool force = false);
    void load_or_generate_identity(const std::string& username_hint = "");

    void process_server_messages();
    void handle_server_message(protocol::ControlMessageType type, const uint8_t* data, size_t len);

    void do_connect();
    void poll_connecting();
    void finish_connect();
    void send_auth_identity();
    void on_disconnect_cleanup();

    void disconnect_intentionally();
    void attempt_reconnect();
    void cancel_reconnect();

    void setup_model_callbacks();
    void setup_chat_model_callbacks();
    void setup_server_model_callbacks();

    void join_channel(ChannelId id);
    void leave_channel();

private:
    PlatformBridge bridge_;

    struct SharerInfo { UserId user_id = 0; std::string name; };
    std::unordered_map<UserId, SharerInfo> active_sharers_;

    std::atomic<UserId> viewing_sharer_{0};
    std::atomic<bool>   awaiting_keyframe_{false};

    ReorderBuffers reorder_;
    PliLimiter     pli_;
    void route_video_stream_frame(UserId sender, std::vector<uint8_t>&& wire);
    void route_video_stream_aborted(UserId sender, uint32_t frame_seq, bool have_seq);
    void dispatch_video(UserId sender, ReorderOutcome&& outcome);
    void retry_pending_plis();
    void feed_video_sender_stats();

    void on_screen_share_viewer(const uint8_t* data, size_t len);

    mutable std::mutex watched_mutex_;
    std::unordered_set<UserId> watched_;
    MultiStreamVideo* multi_stream_video() { return std::get_if<MultiStreamVideo>(&bridge_.video); }
    bool multi_stream() const { return std::holds_alternative<MultiStreamVideo>(bridge_.video); }
    void publish_audio_devices();
    std::unordered_set<UserId> watched_snapshot() const;
    void rebuild_watched_model();

    std::unordered_map<UserId, std::chrono::steady_clock::time_point> voice_last_active_;

    struct PendingPref {
        std::string value;
        std::chrono::steady_clock::time_point updated;
    };
    std::unordered_map<std::string, PendingPref> pending_prefs_;

    struct UserAudioPrefs {
        float voice_volume = 1.0f;
        float music_volume = 1.0f;
        bool compression = false;
        float compression_target = 0.8f;
    };
    std::mutex user_audio_prefs_mutex_;
    std::unordered_map<UserId, UserAudioPrefs> user_audio_prefs_;
    void apply_cached_user_audio_prefs(UserId user_id);

    std::chrono::steady_clock::time_point stream_fps_last_update_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point ping_sent_at_{};
    std::chrono::steady_clock::time_point ping_last_send_{};
    bool ping_pending_ = false;

#ifdef _WIN32
    std::unique_ptr<class AutoUpdater> updater_;
#endif

    struct PendingUpload {
        std::string path;
        std::vector<uint8_t> data;
    };
    std::vector<PendingUpload> pending_uploads_;

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
    void on_screen_share_started(const uint8_t* data, size_t len);
    void on_screen_share_stopped(const uint8_t* data, size_t len);
    void on_screen_share_denied();
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

    IdleSleepInhibitor sleep_inhibitor_;
    void apply_sleep_inhibit(bool wanted);

    struct QueryTarget { int id; std::string host; uint16_t port; };
    struct QueryResult { bool online = false; uint16_t cur = 0; uint16_t max = 0; bool locked = false; std::string name; };

    std::thread               query_thread_;
    std::atomic<bool>         query_thread_run_{false};
    ServerQueryWakeEvent      query_wake_;
    std::atomic<bool>         lobby_visible_{true};
    std::atomic<bool>         query_results_dirty_{false};
    std::mutex                query_targets_mutex_;
    std::vector<QueryTarget>  query_targets_;
    std::mutex                query_results_mutex_;
    std::unordered_map<int, QueryResult> query_results_;

    void start_query_thread();
    void stop_query_thread();
    void query_thread_loop();
    void apply_query_results();
};

}
