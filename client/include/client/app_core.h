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

struct PlatformBridge {
    std::function<void(const std::string&)>                    copy_to_clipboard;
    std::function<void(SoundPlayer::Effect)>                   play_sound;
    std::function<void(float)>                                 set_notification_volume;
    std::function<void(const UserContextWindowRequest&)>       show_user_menu;
    std::function<void(int channel_id, const std::string& name)> show_channel_menu;
    std::function<void(int server_id)>                         show_server_menu;
    std::function<void(int64_t message_id)>                    show_message_menu;
    std::function<void()>                                      open_share_picker;
    std::function<void()>                                      open_audio_share_picker;
    std::function<void()>                                      on_authenticated;
    std::function<void()>                                      stop_screen_share;
    std::function<void()>                                      stop_audio_share;
    // Force the next encoded frame to be a keyframe. AppCore applies the
    // VIDEO_KEYFRAME_REQUEST_COOLDOWN_MS gate before calling this; may be
    // invoked on the MsQuic worker thread or the main thread.
    std::function<void()>                                      request_keyframe;
    // True while the decode pipeline for this watched sharer is still waiting
    // for a keyframe (Windows: VideoDecodeGate; Apple: awaiting_keyframe_).
    // AppCore::tick() re-sends a PLI every VIDEO_PLI_RETRY_MS while this holds.
    std::function<bool(UserId)>                                stream_awaiting_keyframe;
    std::function<void()>                                      clear_video_element;
    // Per-sharer video decode (set only on platforms with a multi-stream decode
    // pipeline — i.e. Windows). When start_video_stream is set, the core treats
    // watching as ADDITIVE (multiple grid streams); when unset (macOS/iOS, single
    // hardware decoder) watching is single-select.
    std::function<void(UserId)>                                start_video_stream;
    std::function<void(UserId)>                                stop_video_stream;
    std::function<void(bool)>                                  set_keep_awake;
    // Camera publishing — set only on platforms that can capture one; the
    // camera controls stay hidden elsewhere. start_camera opens the device
    // selected in model_.selected_camera asynchronously and reports back on
    // the main thread through AppCore::camera_started / camera_failed; it
    // returns false when it could not even begin. stop_camera is idempotent
    // and must leave nothing running.
    std::function<bool()>                                      start_camera;
    std::function<void()>                                      stop_camera;
    // The device or quality changed while the camera is on: reopen the capture
    // underneath the announced stream (viewers keep it; a failure is reported
    // through AppCore::camera_failed, which ends the stream).
    std::function<void()>                                      restart_camera;
    // Force the camera encoder's next frame to be a keyframe (same cooldown and
    // threads as request_keyframe).
    std::function<void()>                                      request_camera_keyframe;
    // Capture device names, in the index order start_camera uses.
    std::function<std::vector<std::string>()>                  list_cameras;
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
    VoiceMixer        aux_mixer_{false};   // secondary stream: no voice makeup/normalization
    Settings          settings_;
    LobbyModel        model_;
    ChatModel         chat_model_;
    ServerListModel   server_model_;
    StreamAudioPlayer stream_audio_player_;

    // Shared state. authenticated_ and server_protocol_version_ are also read
    // by the encoder threads (send_video_frame), hence atomic.
    std::atomic<bool> authenticated_{false};
    UserId      user_id_              = 0;
    std::string global_name_;          // launcher profile/default for new servers
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
    uint16_t    voice2_seq_            = 0;   // secondary (VOICE2) stream sequence
    // Read on the MsQuic worker thread (video receive) and written on the main
    // thread (watch/stop) — must be atomic.
    std::atomic<UserId> viewing_sharer_{0};
    std::atomic<bool>   awaiting_keyframe_{false};
    // Sharer frame counter. Owned by AppCore::send_video_frame (stamped and
    // advanced there, on the encode thread); reset only at share start
    // (reset_video_sender) and on disconnect — never mid-share, or viewers'
    // reorder buffers would see a backward jump.
    std::atomic<uint32_t> video_frame_number_{0};
    std::atomic<uint32_t> stream_frame_count_{0};
    // Protocol version the server reported in AUTH_RESPONSE (1.2+); older
    // servers report nothing and are assumed PROTOCOL_VERSION_ASSUMED_LEGACY.
    std::atomic<uint16_t> server_protocol_version_{protocol::PROTOCOL_VERSION_ASSUMED_LEGACY};

    // ── Video senders (shared by Windows/macOS platform code) ───────────────
    // One per protocol::VideoSource, each with its own frame counter:
    // congestion control (admission on the capture thread + AIMD bitrate).
    // The source-less overloads below are the screen share's.
    VideoSendController video_sender_;    // screen share
    VideoSendController camera_sender_;   // camera
    std::atomic<uint32_t> camera_frame_number_{0};
    VideoSendController& video_sender_for(protocol::VideoSource source) {
        return source == protocol::VideoSource::Camera ? camera_sender_ : video_sender_;
    }

    // Stream start: clear counters, seed the controller with the user's target.
    void reset_video_sender(uint32_t target_bitrate_bps);
    void reset_video_sender(protocol::VideoSource source, uint32_t target_bitrate_bps);
    // UI slider (main thread). The adapted bitrate never exceeds this.
    void set_video_target_bitrate(uint32_t target_bitrate_bps);
    // Capture-thread gate: true = encode this frame. Atomics only.
    bool video_admit_frame();
    bool video_admit_frame(protocol::VideoSource source);
    // Encode thread: bitrate the encoder should switch to (once per change).
    std::optional<uint32_t> take_video_bitrate_update();
    std::optional<uint32_t> take_video_bitrate_update(protocol::VideoSource source);
    // Encode thread: true once when a keyframe must be forced (send failure or
    // an incoming PLI that passed the cooldown).
    bool take_video_keyframe_request();
    bool take_video_keyframe_request(protocol::VideoSource source);

    struct VideoSendResult {
        uint32_t frame_seq = 0;   // sequence stamped into the header (always assigned)
        bool     sent = false;    // false = frame not queued (not connected / send failed)
    };
    // Encode-thread entry point used by the platform encoders. Builds the
    // 14-byte VideoFrameHeader, stamps frame_seq, picks the transport (per-frame
    // stream when the server supports it, else stream 1 — screen only) and
    // accounts the bytes in that source's sender. Safe to call when not
    // publishing (returns sent=false).
    VideoSendResult send_video_frame(const uint8_t* encoded, size_t len, bool keyframe,
                                     uint16_t width, uint16_t height, VideoCodecId codec);
    VideoSendResult send_video_frame(protocol::VideoSource source,
                                     const uint8_t* encoded, size_t len, bool keyframe,
                                     uint16_t width, uint16_t height, VideoCodecId codec);
    // Any thread: SCREEN_SHARE_UPDATE for a non-screen source once its encoder
    // knows the real codec and size.
    void send_video_stream_update(protocol::VideoSource source, VideoCodecId codec,
                                  uint16_t width, uint16_t height);

    // ── Camera (main thread) ─────────────────────────────────────────────────
    // The platform can publish a camera and the server speaks protocol 1.4.
    bool camera_supported() const;
    void start_camera();                // camera button on
    // Button off, leave, channel switch, disconnect (idempotent). tell_server
    // = false when the server ended the stream itself (no STOP to answer).
    void stop_camera(bool tell_server = true);
    // Platform -> core once the capture runs (or failed to): announces the
    // stream to the channel and opens the local preview.
    void camera_started();
    void camera_failed(const std::string& reason);
    void restart_camera();              // device/quality changed while on
    void refresh_camera_devices();      // model_.camera_devices from bridge_.list_cameras
    struct CameraPreset { uint16_t width; uint16_t height; uint32_t fps; uint32_t bitrate_bps; };
    // model_.camera_quality: 0 = 480p, 1 = 720p, 2 = 1080p (all 30 fps).
    static CameraPreset camera_preset(int quality);
    // The capture device the user picked, by index into bridge_.list_cameras.
    int selected_camera_index() const { return model_.selected_camera.get(); }
    std::string tofu_pending_fingerprint_;
    bool        tofu_pending_ = false;
    // Set on the MsQuic worker thread when the connection drops; the actual
    // cleanup (which mutates the RmlUi data model) runs on the main thread in tick().
    std::atomic<bool> disconnect_pending_{false};

    // Resumption ticket arrives on the MsQuic worker thread; sqlite (settings_)
    // is not safe to touch off the main thread, so stash the bytes here and
    // persist them from tick().
    std::mutex           pending_ticket_mutex_;
    std::vector<uint8_t> pending_resumption_ticket_;
    bool                 pending_ticket_dirty_ = false;

    // ── Auto-reconnect ──────────────────────────────────────────────────────
    // Set true by intentional disconnects (user action, kick, TOFU reject,
    // shutdown) so the drop handler knows NOT to reconnect. Cleared after each
    // disconnect is processed.
    bool        intentional_disconnect_ = false;
    bool        reconnecting_           = false;  // a reconnect cycle is in progress
    int         reconnect_attempts_     = 0;      // pre-auth connect failures (backoff)
    int         reconnect_flaps_        = 0;      // auth-succeeds-then-drops cycles (loop guard)
    ChannelId   reconnect_channel_      = 0;      // voice channel to rejoin after re-auth
    bool        rejoin_pending_         = false;  // re-auth succeeded → join reconnect_channel_
    std::chrono::steady_clock::time_point reconnect_next_attempt_{};
    std::chrono::steady_clock::time_point auth_success_time_{};  // last successful auth

    // Actions. Every id below is a stream id (protocol::video_stream_id): a
    // screen share's is its owner's user id, a camera's sets the source bits.
    void watch_sharer(UserId id);   // single-select (replace) — used by chips/cards + single-decoder platforms
    void stop_watching();           // stop watching ALL streams
    void add_watch(UserId id);      // start watching one more stream (additive when supported)
    void remove_watch(UserId id);   // stop watching one stream
    void toggle_watch(UserId id);   // add if not watched, else remove
    // Participant card: open everything this user is publishing (screen and
    // camera; the screen when only one stream can be shown at a time).
    void watch_user(UserId user_id);
    bool is_watching(UserId id) const;
    // Force the watched set to exactly {id} (or empty if 0) and refresh the grid
    // model. For platforms whose watch flow lives in platform code (macOS/iOS,
    // single hardware decoder) rather than going through add_watch/remove_watch.
    void set_single_watched(UserId id);
    void send_voice_state();
    // Single PLI funnel: every keyframe request toward a sharer (decode-gate
    // discontinuity, reorder-buffer loss, tick() retry) goes through here and
    // is rate-limited per target to VIDEO_PLI_COOLDOWN_MS. Thread-safe.
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

    // Auto-reconnect
    void disconnect_intentionally();   // user/explicit disconnect — no reconnect
    void attempt_reconnect();          // fire one reconnect attempt (called from tick)
    void cancel_reconnect();           // abandon the reconnect cycle

    // Model callbacks (called from init())
    void setup_model_callbacks();
    void setup_chat_model_callbacks();
    void setup_server_model_callbacks();

    // Channel operations (public so platform can call join_channel if needed)
    void join_channel(ChannelId id);
    // tell_server = false: the server already took us out (channel deleted).
    void leave_channel(bool tell_server = true);

private:
    PlatformBridge bridge_;

    struct SharerInfo { UserId user_id = 0; std::string name; };
    std::unordered_map<UserId, SharerInfo> active_sharers_;

    // ── Screen-share receive (per-frame streams) ────────────────────────────
    // One reorder buffer per watched sharer. Fed on the MsQuic worker thread
    // (net_.on_video_stream_frame / on_video_stream_aborted) and polled from
    // tick(); guarded by reorder_mutex_. Delivered frames go to
    // on_video_frame_received with the same [header][encoded] layout the
    // stream-1 path produces. Erased in remove_watch / stop_watching /
    // clear_all_sharers / on_disconnect_cleanup.
    std::mutex reorder_mutex_;
    std::unordered_map<UserId, VideoFrameReorderBuffer> reorder_buffers_;
    void route_video_stream_frame(uint32_t sender_id, std::vector<uint8_t>&& frame);
    void route_video_stream_aborted(uint32_t sender_id, uint32_t frame_seq, bool have_seq);
    void poll_video_reorder_buffers();
    void erase_reorder_buffer(UserId sharer_id);

    // PLI cooldown per target (send_pli) and keyframe-request cooldown for
    // incoming PLIs per source (bridge_.request_keyframe /
    // request_camera_keyframe). Both touched from the MsQuic thread and the
    // main thread; guarded by pli_mutex_.
    std::mutex pli_mutex_;
    std::unordered_map<UserId, std::chrono::steady_clock::time_point> pli_last_sent_;
    std::chrono::steady_clock::time_point keyframe_request_last_{};
    std::chrono::steady_clock::time_point camera_keyframe_request_last_{};
    void on_keyframe_request(protocol::VideoSource source);   // incoming PLI (MsQuic thread)

    // Camera publishing state (main thread).
    bool camera_starting_        = false;   // start issued, camera_started not seen yet
    bool camera_preview_pending_ = false;   // open our own preview once the server confirms
    bool camera_restart_pending_ = false;   // device/quality changed while still opening
    // SCREEN_SHARE_STOPs for our camera the server has not echoed yet: an
    // echo matching one is ours; any other STOPPED means the server ended it.
    int  camera_stops_in_flight_ = 0;
    void sync_camera_supported();

    // The watched screen share whose stream audio plays (StreamAudioPlayer is
    // a single decoder). Cameras carry no audio, so a camera never takes the
    // focus. Read on the MsQuic worker thread.
    std::atomic<UserId> audio_sharer_{0};
    void refocus_stream_audio();           // after a removal: any watched screen, else none
    void retry_pending_plis();          // tick(): re-send while a stream awaits a keyframe
    void feed_video_sender_stats();     // tick(): QUIC stats -> video_sender_

    void on_screen_share_viewer(const uint8_t* data, size_t len);   // SCREEN_SHARE_VIEWER (sound)

    // The set of sharers currently being watched (shown in the viewer grid).
    // Authoritative watch state; mirrored into model_.watched for the UI.
    mutable std::mutex watched_mutex_;
    std::unordered_set<UserId> watched_;
    bool multi_stream() const { return static_cast<bool>(bridge_.start_video_stream); }
    void rebuild_watched_model();   // refresh model_.watched + sharers[].watching + counts

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
    // Stream creation happens on the QUIC worker thread. It may only consult
    // this cache; SQLite access remains on the main thread.
    void apply_cached_user_audio_prefs(UserId user_id);

    std::chrono::steady_clock::time_point stream_fps_last_update_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point ping_sent_at_{};
    std::chrono::steady_clock::time_point ping_last_send_{};
    bool ping_pending_ = false;
    // Last NetClient::datagram_backlog().dropped value written to the log.
    std::chrono::steady_clock::time_point datagram_drop_log_last_{};
    uint64_t datagram_drops_logged_ = 0;

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

    IdleSleepInhibitor sleep_inhibitor_;
    void apply_sleep_inhibit(bool wanted);

    // ── Server-list query polling (connectionless, game-browser style) ──
    // A background thread queries each saved server's UDP port while the lobby
    // is visible; results are applied to server_model_ on the main thread in
    // tick() (mirrors the disconnect_pending_ cross-thread pattern).
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

} // namespace parties::client
