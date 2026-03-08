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
    Rml::String server_name;
    Rml::String username;
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
    float normalize_target = 0.8f;
    bool aec_enabled = false;
    bool vad_enabled = false;
    float vad_threshold = 0.02f;
    float voice_level = 0.0f;

    // Push-to-talk
    bool ptt_enabled = false;
    int ptt_key = 0;                // Win32 virtual key code (0 = not set)
    Rml::String ptt_key_name;       // display name for UI
    bool ptt_binding = false;        // true when waiting for key press
    float ptt_delay = 0.0f;         // release delay in ms (0-1000, step 50)

    // Screen sharing
    bool is_sharing = false;
    bool someone_sharing = false;           // convenience: !sharers.empty()
    Rml::Vector<ActiveSharer> sharers;      // all active sharers in channel
    int viewing_sharer_id = 0;              // who we're subscribed to (0 = none)
    float stream_volume = 1.0f;             // stream audio volume (0.0 - 2.0)

    // Share picker
    bool show_share_picker = false;
    Rml::Vector<ShareTarget> share_targets;
    int share_codec = 0;           // 0=Auto, 1=H.265, 2=H.264
    float share_bitrate = 2.0f;    // Mbps (0.5 - 10.0)

    // Admin / permissions
    int my_role = 3;                       // current user's role (0=Owner..3=User)
    bool can_manage_channels = false;      // derived from role
    bool can_kick = false;                 // derived from role
    bool can_manage_roles = false;         // derived from role

    // Create channel form
    bool show_create_channel = false;
    Rml::String new_channel_name;

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
    std::function<void()>      on_toggle_ptt;
    std::function<void()>      on_ptt_bind;
    std::function<void(float)> on_ptt_delay_changed;
    std::function<void()>      on_toggle_share;
    std::function<void(int)>   on_select_share_target;
    std::function<void()>      on_cancel_share;
    std::function<void(int)>   on_watch_sharer;
    std::function<void(int)>   on_select_sharer;
    std::function<void()>      on_stop_watching;
    std::function<void(float)> on_stream_volume_changed;

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
    std::function<void(int, std::string, int)> on_show_user_menu;    // (user_id, name, role)
    std::function<void(int, std::string)>      on_show_channel_menu; // (channel_id, name)

private:
    Rml::DataModelHandle handle_;
};

} // namespace parties::client
