#pragma once

#include <parties/types.h>

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

struct ChannelUser {
    Rml::String name;
    int id = 0;
    int role = 0;
    bool muted = false;
    bool deafened = false;
    bool speaking = false;
    bool streaming = false;
    int color_index = 0;  // 0-11, generated from name hash for avatar color
};

struct ChannelInfo {
    int id = 0;
    Rml::String name;
    int user_count = 0;
    int max_users = 0;
    Rml::Vector<ChannelUser> users;
};

struct AudioDevice {
    Rml::String name;
    int index = 0;
};

struct ShareTarget {
    Rml::String name;
    int index = 0;       // index into App's capture_targets_ vector
    bool is_monitor = false;
};

struct ActiveSharer {
    int id = 0;
    Rml::String name;
};

class LobbyModel {
public:
    LobbyModel();
    ~LobbyModel();

    bool init(Rml::Context* context);

    void dirty(const Rml::String& variable);
    void dirty_all();

    // --- Bound state (public for App to update directly) ---
    bool is_connected = false;
    int ping_ms = 0;
    Rml::String server_name;
    Rml::String server_initials;
    int server_color_index = 0;
    Rml::String username;
    int my_color_index = 0;   // user's avatar color (0-11), from fingerprint hash
    Rml::String error_text;

    Rml::Vector<ChannelInfo> channels;
    int current_channel = 0;
    Rml::String current_channel_name;

    bool is_muted = false;
    bool is_deafened = false;
    bool show_settings = false;

    // Audio settings
    Rml::Vector<AudioDevice> capture_devices;
    Rml::Vector<AudioDevice> playback_devices;
    int selected_capture = 0;
    int selected_playback = 0;
    bool denoise_enabled = true;
    bool normalize_enabled = false;
    float normalize_target = 0.5f;
    bool aec_enabled = false;
    bool vad_enabled = true;
    float vad_threshold = 0.10f;
    float voice_level = 0.0f;

    // Notification sounds
    float notification_volume = 1.0f;   // 0.0 - 2.0

    // Push-to-talk
    bool ptt_enabled = false;
    int ptt_key = 0;                // Win32 virtual key code (0 = not set)
    int ptt_key2 = 0;               // optional second regular key held in combo (0 = none)
    int ptt_mods = 0;               // modifier bitmask: 1=Ctrl 2=Shift 4=Alt
    Rml::String ptt_key_name;       // display name for UI
    bool ptt_binding = false;        // true when waiting for key press
    float ptt_delay = 0.0f;         // release delay in ms (0-1000, step 50)

    // Global hotkeys
    int mute_key = 0;               // Toggle mute hotkey (0 = not set)
    int mute_key2 = 0;
    int mute_mods = 0;
    Rml::String mute_key_name;
    bool mute_binding = false;
    int deafen_key = 0;             // Toggle deafen hotkey (0 = not set)
    int deafen_key2 = 0;
    int deafen_mods = 0;
    Rml::String deafen_key_name;
    bool deafen_binding = false;

    // Mobile navigation (iOS: sidebar vs content panel)
    bool mobile_show_content = false;

    // Chat view active (hides voice channel-content area)
    bool show_chat = false;

    // Screen sharing
    bool is_sharing = false;
    bool someone_sharing = false;           // convenience: !sharers.empty()
    Rml::Vector<ActiveSharer> sharers;      // all active sharers in channel
    int viewing_sharer_id = 0;              // who we're subscribed to (0 = none)
    float stream_volume = 1.0f;             // stream audio volume (0.0 - 2.0)
    bool stream_fullscreen = false;         // double-click toggles fullscreen stream view
    int stream_fps = 0;                     // current stream FPS (encode or decode)

    // Share picker
    bool show_share_picker = false;
    bool use_native_picker = false;  // true on macOS (native picker, no target list)
    Rml::Vector<ShareTarget> share_targets;
    float share_bitrate = 2.0f;    // Mbps (0.5 - 20.0)
    int share_fps = 2;             // 0=15, 1=30, 2=60, 3=120
    int share_codec = 0;           // 0=AV1, 1=H.265, 2=H.264
    int share_scale = 0;           // 0=Source, 1=x0.75, 2=x0.5, 3=x0.25

    // Auto-update
    bool update_available = false;
    bool update_downloading = false;
    bool update_ready = false;
    Rml::String update_version;

    // Admin / permissions
    int my_role = 3;                       // current user's role (0=Owner..3=User)
    bool can_manage_channels = false;      // derived from role
    bool can_kick = false;                 // derived from role
    bool can_manage_roles = false;         // derived from role

    // User context menu (RmlUi-based, non-blocking)
    bool show_user_menu = false;
    int menu_user_id = 0;
    Rml::String menu_user_name;
    int menu_user_role = 0;
    float menu_user_volume = 1.0f;         // 0.0 - 2.0
    bool menu_user_compress = false;       // per-user voice compression enabled
    float menu_user_compress_target = 0.8f; // compression target (0.0 - 1.0)
    bool menu_can_roles = false;           // can we change this user's role?
    bool menu_can_kick = false;            // can we kick this user?

    // Create channel form
    bool show_create_channel = false;
    Rml::String new_channel_name;

    // Rename channel form
    bool show_rename_channel = false;
    int rename_channel_id = 0;
    Rml::String rename_channel_name;      // current name (display only)
    Rml::String new_rename_channel_name;  // input field

    // Admin feedback
    Rml::String admin_message;

    // Identity backup/import/export
    bool show_seed_phrase = false;
    Rml::String identity_seed_phrase;
    bool show_import_identity = false;
    Rml::String import_phrase;
    Rml::String import_error;
    bool show_private_key = false;
    Rml::String identity_private_key;  // hex-encoded 32-byte Ed25519 seed

    // --- Callbacks (set by App before init) ---
    std::function<void()>      on_disconnect_server;
    std::function<void(int)>   on_join_channel;
    std::function<void()>      on_leave_channel;
    std::function<void()>      on_toggle_mute;
    std::function<void()>      on_toggle_deafen;
    std::function<void(int)>   on_select_capture;
    std::function<void(int)>   on_select_playback;
    std::function<void(bool)>  on_denoise_changed;
    std::function<void(bool)>  on_normalize_changed;
    std::function<void(float)> on_normalize_target_changed;
    std::function<void(bool)>  on_aec_changed;
    std::function<void(bool)>  on_vad_changed;
    std::function<void(float)> on_vad_threshold_changed;
    std::function<void(float)> on_notification_volume_changed;
    std::function<void()>      on_test_notification_sound;
    std::function<void()>      on_toggle_ptt;
    std::function<void()>      on_ptt_bind;
    std::function<void(float)> on_ptt_delay_changed;
    std::function<void()>      on_mute_bind;
    std::function<void()>      on_deafen_bind;
    std::function<void()>      on_toggle_share;
    std::function<void(int)>   on_select_share_target;
    std::function<void()>      on_cancel_share;
    std::function<void()>      on_start_native_share;  // macOS: trigger native picker
    std::function<void(float)> on_share_bitrate_changed;
    std::function<void(int)>   on_watch_sharer;
    std::function<void(int)>   on_select_sharer;
    std::function<void()>      on_stop_watching;
    std::function<void(float)> on_stream_volume_changed;
    std::function<void()>      on_stream_tap_fullscreen;  // iOS: single tap toggles fullscreen

    // Auto-update
    std::function<void()>      on_apply_update;

    // Identity
    std::function<void()>      on_show_seed_phrase;
    std::function<void()>      on_copy_seed_phrase;
    std::function<void()>      on_show_private_key;
    std::function<void()>      on_copy_private_key;
    std::function<void()>      on_show_import;
    std::function<void()>      on_do_import;
    std::function<void()>      on_cancel_import;

    // Admin
    std::function<void()>      on_create_channel;
    std::function<void(int)>   on_delete_channel;
    std::function<void()>      on_rename_channel;
    std::function<void(int, std::string, int)> on_show_user_menu;    // (user_id, name, role)
    std::function<void(int, std::string)>      on_show_channel_menu; // (channel_id, name)

    // User context menu actions
    std::function<void(int, int)>    on_set_user_role;       // (user_id, new_role)
    std::function<void(int)>         on_kick_user;           // (user_id)
    std::function<void(int, float)>  on_user_volume_changed; // (user_id, volume)
    std::function<void(int, bool, float)> on_user_compress_changed; // (user_id, enabled, target)

private:
    Rml::DataModelHandle handle_;
};

} // namespace parties::client
