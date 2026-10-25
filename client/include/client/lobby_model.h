#pragma once

#include <parties/types.h>
#include <client/document_router.h>
#include <client/rml_binding.h>

#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

namespace rml = parties::rml;

struct ChannelUser {
    Rml::String name;
    int id = 0;
    int role = 0;
    bool muted = false;
    bool deafened = false;
    bool speaking = false;
    bool music_playing = false;
    bool streaming = false;
    int color_index = 0;
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
    int index = 0;
    bool is_monitor = false;
    Rml::String element_id;
};

struct ActiveSharer {
    int id = 0;
    Rml::String name;
    bool watching = false;
};

struct WatchedStream {
    int id = 0;
    Rml::String name;
    Rml::String element_id;
};

class LobbyModel : public rml::Model {
public:
    void show_voice_channel(int channel_id);

    bool add_channel_sharer(int user_id);
    void remove_channel_sharer(int user_id);
    void clear_channel_sharers();

    rml::Prop<bool>        is_connected{false};
    rml::Prop<int>         ping_ms{0};
    rml::Prop<Rml::String> server_name;
    rml::Prop<Rml::String> server_initials;
    rml::Prop<int>         server_color_index{0};
    rml::Prop<Rml::String> username;
    rml::Prop<int>         my_color_index{0};
    rml::Prop<Rml::String> error_text;

    rml::Prop<Rml::Vector<ChannelInfo>> channels;
    rml::Prop<int>         current_channel{0};
    rml::Prop<Rml::String> current_channel_name;

    rml::Prop<bool>        is_muted{false};
    rml::Prop<bool>        is_deafened{false};
    DocumentRouter         router;

    rml::Prop<Rml::Vector<AudioDevice>> capture_devices;
    rml::Prop<Rml::Vector<AudioDevice>> playback_devices;
    rml::Prop<int>         selected_capture{0};
    rml::Prop<int>         selected_playback{0};
    rml::Prop<bool>        denoise_enabled{true};
    rml::Prop<bool>        normalize_enabled{false};
    rml::Prop<float>       normalize_target{0.5f};
    rml::Prop<bool>        aec_enabled{false};
    rml::Prop<bool>        vad_enabled{true};
    rml::Prop<float>       vad_threshold{0.10f};
    rml::Prop<float>       voice_level{0.0f};

    rml::Prop<float>       voice_volume{1.0f};
    rml::Prop<float>       secondary_volume{1.0f};
    rml::Prop<float>       music_send_volume{1.0f};

    rml::Prop<float>       notification_volume{1.0f};

    rml::Prop<bool>        ptt_enabled{false};
    rml::Prop<int>         ptt_key{0};
    int ptt_key2 = 0;
    int ptt_mods = 0;
    rml::Prop<Rml::String> ptt_key_name;
    rml::Prop<bool>        ptt_binding{false};
    rml::Prop<float>       ptt_delay{0.0f};
    rml::Prop<Rml::String> ptt_delay_text{"0 ms"};

    rml::Prop<int>         mute_key{0};
    int mute_key2 = 0;
    int mute_mods = 0;
    rml::Prop<Rml::String> mute_key_name;
    rml::Prop<bool>        mute_binding{false};
    rml::Prop<int>         deafen_key{0};
    int deafen_key2 = 0;
    int deafen_mods = 0;
    rml::Prop<Rml::String> deafen_key_name;
    rml::Prop<bool>        deafen_binding{false};

    rml::Prop<bool>        mobile_show_content{false};

    rml::Prop<bool>        is_sharing{false};
    rml::Prop<bool>        is_audio_sharing{false};
    rml::Prop<Rml::String> audio_share_target_name;
    rml::Prop<bool>        someone_sharing{false};
    rml::Prop<Rml::Vector<ActiveSharer>> sharers;
    rml::Prop<Rml::Vector<WatchedStream>> watched;
    rml::Prop<int>         watching_count{0};
    rml::Prop<int>         viewing_sharer_id{0};
    rml::Prop<float>       stream_volume{1.0f};
    rml::Prop<bool>        stream_fullscreen{false};
    rml::Prop<int>         stream_fps{0};
    rml::Prop<int>         pip_stream_id{0};
    rml::Prop<bool>        pip_supported{false};

    rml::Prop<bool>        use_native_picker{false};
    // Keep monitor and application targets in separate collections. RmlUi's
    // data-if hides non-matching data-for rows but keeps their elements in the
    // DOM, so filtering one combined collection in markup creates duplicate
    // preview ids and routes frames into hidden video elements.
    rml::Prop<Rml::Vector<ShareTarget>> share_monitor_targets;
    rml::Prop<Rml::Vector<ShareTarget>> share_application_targets;
    rml::Prop<int>         selected_share_target{-1};
    rml::Prop<int>         share_picker_mode{0};
    rml::Prop<float>       share_bitrate{2.0f};
    rml::Prop<int>         share_fps{2};
    rml::Prop<int>         share_codec{0};
    rml::Prop<int>         share_scale{0};
    rml::Prop<int>         share_preset{-1};

    rml::Prop<bool>        update_available{false};
    rml::Prop<bool>        update_downloading{false};
    rml::Prop<bool>        update_ready{false};
    rml::Prop<Rml::String> update_version;

    rml::Prop<int>         my_role{3};
    rml::Prop<bool>        can_manage_channels{false};
    rml::Prop<bool>        can_kick{false};
    rml::Prop<bool>        can_manage_roles{false};

    rml::Prop<bool>        show_user_menu{false};
    rml::Prop<int>         menu_user_id{0};
    rml::Prop<Rml::String> menu_user_name;
    rml::Prop<int>         menu_user_role{0};
    rml::Prop<float>       menu_user_volume{1.0f};
    rml::Prop<float>       menu_user_music_volume{1.0f};
    rml::Prop<bool>        menu_user_compress{false};
    rml::Prop<float>       menu_user_compress_target{0.8f};
    rml::Prop<bool>        menu_can_roles{false};
    rml::Prop<bool>        menu_can_kick{false};

    rml::Prop<bool>        show_create_channel{false};
    rml::Prop<Rml::String> new_channel_name;

    rml::Prop<bool>        show_rename_channel{false};
    rml::Prop<int>         rename_channel_id{0};
    rml::Prop<Rml::String> rename_channel_name;
    rml::Prop<Rml::String> new_rename_channel_name;

    rml::Prop<Rml::String> admin_message;

    rml::Prop<bool>        show_seed_phrase{false};
    rml::Prop<Rml::String> identity_seed_phrase;
    rml::Prop<bool>        show_import_identity{false};
    rml::Prop<Rml::String> import_phrase;
    rml::Prop<Rml::String> import_error;
    rml::Prop<bool>        show_private_key{false};
    rml::Prop<Rml::String> identity_private_key;

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
    std::function<void(float)> on_voice_volume_changed;
    std::function<void(float)> on_secondary_volume_changed;
    std::function<void(float)> on_music_send_volume_changed;
    std::function<void(float)> on_notification_volume_changed;
    std::function<void()>      on_test_notification_sound;
    std::function<void()>      on_toggle_ptt;
    std::function<void()>      on_ptt_bind;
    std::function<void(float)> on_ptt_delay_changed;
    std::function<void()>      on_mute_bind;
    std::function<void()>      on_deafen_bind;
    std::function<void()>      on_toggle_share;
    std::function<void()>      on_toggle_audio_share;
    std::function<void(int)>   on_select_share_target;
    std::function<void()>      on_cancel_share;
    std::function<void()>      on_start_native_share;
    std::function<void(float)> on_share_bitrate_changed;
    std::function<void(int)>   on_watch_sharer;
    std::function<void(int)>   on_select_sharer;
    std::function<void(int)>   on_toggle_watch;
    std::function<void()>      on_stop_watching;
    std::function<void(float)> on_stream_volume_changed;
    std::function<void()>      on_stream_tap_fullscreen;
    std::function<void(int)>   on_toggle_stream_pip;

    std::function<void()>      on_apply_update;

    std::function<void()>      on_show_seed_phrase;
    std::function<void()>      on_copy_seed_phrase;
    std::function<void()>      on_show_private_key;
    std::function<void()>      on_copy_private_key;
    std::function<void()>      on_show_import;
    std::function<void()>      on_do_import;
    std::function<void()>      on_cancel_import;

    std::function<void()>      on_create_channel;
    std::function<void(int)>   on_delete_channel;
    std::function<void()>      on_rename_channel;
    std::function<void(int, std::string, int)> on_show_user_menu;
    std::function<void(int, std::string)>      on_show_channel_menu;

    std::function<void(int, int)>    on_set_user_role;
    std::function<void(int)>         on_kick_user;
    std::function<void(int, float)>  on_user_volume_changed;
    std::function<void(int, float)>  on_user_music_volume_changed;
    std::function<void(int, bool, float)> on_user_compress_changed;

protected:
    const char* model_name() const override { return "lobby"; }
    void build(rml::Builder& b) override;
};

}
