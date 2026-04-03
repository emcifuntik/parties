#include <client/lobby_model.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

namespace parties::client {

LobbyModel::LobbyModel() = default;
LobbyModel::~LobbyModel() = default;

bool LobbyModel::init(Rml::Context* context) {
    Rml::DataModelConstructor ctor = context->CreateDataModel("lobby");
    if (!ctor)
        return false;

    // Register struct + array types
    // Order matters: array types must be registered BEFORE structs that contain them as members.
    // ChannelInfo has a Vector<ChannelUser> member, so register ChannelUser + its array first.

    if (auto s = ctor.RegisterStruct<ChannelUser>()) {
        s.RegisterMember("name",     &ChannelUser::name);
        s.RegisterMember("id",       &ChannelUser::id);
        s.RegisterMember("role",     &ChannelUser::role);
        s.RegisterMember("muted",    &ChannelUser::muted);
        s.RegisterMember("deafened", &ChannelUser::deafened);
        s.RegisterMember("speaking",    &ChannelUser::speaking);
        s.RegisterMember("streaming",   &ChannelUser::streaming);
        s.RegisterMember("color_index", &ChannelUser::color_index);
    }
    ctor.RegisterArray<Rml::Vector<ChannelUser>>();

    if (auto s = ctor.RegisterStruct<ChannelInfo>()) {
        s.RegisterMember("id",         &ChannelInfo::id);
        s.RegisterMember("name",       &ChannelInfo::name);
        s.RegisterMember("user_count", &ChannelInfo::user_count);
        s.RegisterMember("max_users",  &ChannelInfo::max_users);
        s.RegisterMember("users",      &ChannelInfo::users);
    }
    ctor.RegisterArray<Rml::Vector<ChannelInfo>>();

    if (auto s = ctor.RegisterStruct<AudioDevice>()) {
        s.RegisterMember("name",  &AudioDevice::name);
        s.RegisterMember("index", &AudioDevice::index);
    }
    ctor.RegisterArray<Rml::Vector<AudioDevice>>();

    if (auto s = ctor.RegisterStruct<ShareTarget>()) {
        s.RegisterMember("name",       &ShareTarget::name);
        s.RegisterMember("index",      &ShareTarget::index);
        s.RegisterMember("is_monitor", &ShareTarget::is_monitor);
    }
    ctor.RegisterArray<Rml::Vector<ShareTarget>>();

    if (auto s = ctor.RegisterStruct<ActiveSharer>()) {
        s.RegisterMember("id",   &ActiveSharer::id);
        s.RegisterMember("name", &ActiveSharer::name);
    }
    ctor.RegisterArray<Rml::Vector<ActiveSharer>>();

    // Bind variables
    ctor.Bind("is_connected",   &is_connected);
    ctor.Bind("connection_insecure", &connection_insecure);
    ctor.Bind("ping_ms",        &ping_ms);
    ctor.Bind("server_name",    &server_name);
    ctor.Bind("server_initials",&server_initials);
    ctor.Bind("server_color_index", &server_color_index);
    ctor.Bind("username",       &username);
    ctor.Bind("my_color_index", &my_color_index);
    ctor.Bind("error_text",     &error_text);
    ctor.Bind("channels",       &channels);
    ctor.Bind("current_channel",     &current_channel);
    ctor.Bind("current_channel_name",&current_channel_name);
    ctor.Bind("is_muted",       &is_muted);
    ctor.Bind("is_deafened",    &is_deafened);
    ctor.Bind("show_settings",  &show_settings);
    ctor.Bind("show_chat",      &show_chat);

    ctor.Bind("capture_devices",  &capture_devices);
    ctor.Bind("playback_devices", &playback_devices);
    ctor.Bind("selected_capture", &selected_capture);
    ctor.Bind("selected_playback",&selected_playback);
    ctor.Bind("denoise_enabled",  &denoise_enabled);
    ctor.Bind("normalize_enabled",&normalize_enabled);
    ctor.Bind("normalize_target", &normalize_target);
    ctor.Bind("aec_enabled",      &aec_enabled);
    ctor.Bind("vad_enabled",      &vad_enabled);
    ctor.Bind("vad_threshold",    &vad_threshold);
    ctor.Bind("voice_level",      &voice_level);
    ctor.Bind("notification_volume", &notification_volume);
    ctor.Bind("ptt_enabled",      &ptt_enabled);
    ctor.Bind("ptt_key",          &ptt_key);
    ctor.Bind("ptt_key_name",     &ptt_key_name);
    ctor.Bind("ptt_binding",      &ptt_binding);
    ctor.Bind("ptt_delay",        &ptt_delay);
    ctor.Bind("mute_key",         &mute_key);
    ctor.Bind("mute_key_name",    &mute_key_name);
    ctor.Bind("mute_binding",     &mute_binding);
    ctor.Bind("deafen_key",       &deafen_key);
    ctor.Bind("deafen_key_name",  &deafen_key_name);
    ctor.Bind("deafen_binding",   &deafen_binding);

    // Mobile navigation
    ctor.Bind("mobile_show_content", &mobile_show_content);

    // Screen sharing
    ctor.Bind("is_sharing",          &is_sharing);
    ctor.Bind("someone_sharing",     &someone_sharing);
    ctor.Bind("sharers",             &sharers);
    ctor.Bind("viewing_sharer_id",   &viewing_sharer_id);
    ctor.Bind("stream_volume",       &stream_volume);
    ctor.Bind("stream_fullscreen",   &stream_fullscreen);
    ctor.Bind("stream_fps",         &stream_fps);

    // Share picker
    ctor.Bind("show_share_picker", &show_share_picker);
    ctor.Bind("use_native_picker", &use_native_picker);
    ctor.Bind("share_targets",     &share_targets);
    ctor.Bind("share_bitrate",     &share_bitrate);
    ctor.Bind("share_fps",         &share_fps);
    ctor.Bind("share_codec",       &share_codec);
    ctor.Bind("share_scale",       &share_scale);

    ctor.Bind("update_available",   &update_available);
    ctor.Bind("update_downloading", &update_downloading);
    ctor.Bind("update_ready",       &update_ready);
    ctor.Bind("update_version",     &update_version);

    ctor.BindEventCallback("apply_update",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_apply_update) on_apply_update();
        });

    // Admin / permissions
    ctor.Bind("my_role",              &my_role);
    ctor.Bind("can_manage_channels",  &can_manage_channels);
    ctor.Bind("can_kick",             &can_kick);
    ctor.Bind("can_manage_roles",     &can_manage_roles);
    ctor.Bind("show_create_channel",  &show_create_channel);
    ctor.Bind("new_channel_name",     &new_channel_name);
    ctor.Bind("show_rename_channel",      &show_rename_channel);
    ctor.Bind("rename_channel_id",        &rename_channel_id);
    ctor.Bind("rename_channel_name",      &rename_channel_name);
    ctor.Bind("new_rename_channel_name",  &new_rename_channel_name);
    ctor.Bind("admin_message",        &admin_message);

    // User context menu
    ctor.Bind("show_user_menu",     &show_user_menu);
    ctor.Bind("menu_user_id",       &menu_user_id);
    ctor.Bind("menu_user_name",     &menu_user_name);
    ctor.Bind("menu_user_role",     &menu_user_role);
    ctor.Bind("menu_user_volume",   &menu_user_volume);
    ctor.Bind("menu_user_compress", &menu_user_compress);
    ctor.Bind("menu_user_compress_target", &menu_user_compress_target);
    ctor.Bind("menu_can_roles",     &menu_can_roles);
    ctor.Bind("menu_can_kick",      &menu_can_kick);

    // Identity backup/import/export
    ctor.Bind("show_seed_phrase",       &show_seed_phrase);
    ctor.Bind("identity_seed_phrase",   &identity_seed_phrase);
    ctor.Bind("show_import_identity",   &show_import_identity);
    ctor.Bind("import_phrase",          &import_phrase);
    ctor.Bind("import_error",           &import_error);
    ctor.Bind("show_private_key",       &show_private_key);
    ctor.Bind("identity_private_key",   &identity_private_key);

    // Event callbacks
    ctor.BindEventCallback("disconnect_server",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_disconnect_server) on_disconnect_server();
        });

    ctor.BindEventCallback("join_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_join_channel) {
                on_join_channel(args[0].Get<int>());
                mobile_show_content = true;
                dirty("mobile_show_content");
            }
        });

    ctor.BindEventCallback("leave_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_leave_channel) on_leave_channel();
            mobile_show_content = false;
            dirty("mobile_show_content");
        });

    ctor.BindEventCallback("toggle_mute",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_toggle_mute) on_toggle_mute();
        });

    ctor.BindEventCallback("toggle_deafen",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_toggle_deafen) on_toggle_deafen();
        });

    ctor.BindEventCallback("toggle_settings",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_settings = !show_settings;
            mobile_show_content = show_settings;
            dirty("show_settings");
            dirty("mobile_show_content");
        });

    ctor.BindEventCallback("select_capture",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                selected_capture = args[0].Get<int>();
                dirty("selected_capture");
                if (on_select_capture) on_select_capture(selected_capture);
            }
        });

    ctor.BindEventCallback("select_playback",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                selected_playback = args[0].Get<int>();
                dirty("selected_playback");
                if (on_select_playback) on_select_playback(selected_playback);
            }
        });

    ctor.BindEventCallback("toggle_denoise",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            denoise_enabled = !denoise_enabled;
            dirty("denoise_enabled");
            if (on_denoise_changed) on_denoise_changed(denoise_enabled);
        });

    ctor.BindEventCallback("toggle_aec",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            aec_enabled = !aec_enabled;
            dirty("aec_enabled");
            if (on_aec_changed) on_aec_changed(aec_enabled);
        });

    ctor.BindEventCallback("toggle_normalize",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            normalize_enabled = !normalize_enabled;
            dirty("normalize_enabled");
            if (on_normalize_changed) on_normalize_changed(normalize_enabled);
        });

    ctor.BindEventCallback("toggle_vad",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            vad_enabled = !vad_enabled;
            dirty("vad_enabled");
            if (on_vad_changed) on_vad_changed(vad_enabled);
        });

    ctor.BindEventCallback("normalize_target_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_normalize_target_changed) on_normalize_target_changed(normalize_target);
        });

    ctor.BindEventCallback("vad_threshold_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_vad_threshold_changed) on_vad_threshold_changed(vad_threshold);
        });

    ctor.BindEventCallback("notification_volume_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_notification_volume_changed) on_notification_volume_changed(notification_volume);
        });

    ctor.BindEventCallback("test_notification_sound",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_test_notification_sound) on_test_notification_sound();
        });

    ctor.BindEventCallback("toggle_ptt",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_toggle_ptt) on_toggle_ptt();
        });

    ctor.BindEventCallback("ptt_bind",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_ptt_bind) on_ptt_bind();
        });

    ctor.BindEventCallback("mute_bind",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_mute_bind) on_mute_bind();
        });

    ctor.BindEventCallback("deafen_bind",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_deafen_bind) on_deafen_bind();
        });

    ctor.BindEventCallback("ptt_delay_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_ptt_delay_changed) on_ptt_delay_changed(ptt_delay);
        });

    ctor.BindEventCallback("toggle_share",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_toggle_share) on_toggle_share();
        });

    ctor.BindEventCallback("select_share_target",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_select_share_target)
                on_select_share_target(args[0].Get<int>());
        });

    ctor.BindEventCallback("cancel_share",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_cancel_share) on_cancel_share();
        });

    ctor.BindEventCallback("start_native_share",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_start_native_share) on_start_native_share();
        });

    ctor.BindEventCallback("select_share_fps",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                share_fps = args[0].Get<int>();
                dirty("share_fps");
            }
        });

    ctor.BindEventCallback("share_bitrate_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_share_bitrate_changed) on_share_bitrate_changed(share_bitrate);
        });

    ctor.BindEventCallback("select_share_codec",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                share_codec = args[0].Get<int>();
                dirty("share_codec");
            }
        });

    ctor.BindEventCallback("select_share_scale",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                share_scale = args[0].Get<int>();
                dirty("share_scale");
            }
        });

    ctor.BindEventCallback("watch_sharer",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_watch_sharer) {
                on_watch_sharer(args[0].Get<int>());
                mobile_show_content = true;
                dirty("mobile_show_content");
            }
        });

    ctor.BindEventCallback("select_sharer",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_select_sharer)
                on_select_sharer(args[0].Get<int>());
        });

    ctor.BindEventCallback("stop_watching",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_stop_watching) on_stop_watching();
            mobile_show_content = false;
            dirty("mobile_show_content");
        });

    ctor.BindEventCallback("mobile_back",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            mobile_show_content = false;
            show_settings = false;
            dirty("mobile_show_content");
            dirty("show_settings");
            if (on_stop_watching) on_stop_watching();
        });

    ctor.BindEventCallback("stream_volume_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_stream_volume_changed) on_stream_volume_changed(stream_volume);
        });

    ctor.BindEventCallback("toggle_stream_fullscreen",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            stream_fullscreen = !stream_fullscreen;
            dirty("stream_fullscreen");
        });

    ctor.BindEventCallback("stream_tap_fullscreen",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_stream_tap_fullscreen) on_stream_tap_fullscreen();
        });

    // Admin event callbacks
    ctor.BindEventCallback("show_create_channel_form",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_create_channel = true;
            new_channel_name = "";
            dirty("show_create_channel");
            dirty("new_channel_name");
        });

    ctor.BindEventCallback("cancel_create_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_create_channel = false;
            dirty("show_create_channel");
        });

    ctor.BindEventCallback("create_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_create_channel) on_create_channel();
        });

    ctor.BindEventCallback("cancel_rename_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_rename_channel = false;
            dirty("show_rename_channel");
        });

    ctor.BindEventCallback("rename_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_rename_channel) on_rename_channel();
        });

    ctor.BindEventCallback("channel_mousedown",
        [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            int button = ev.GetParameter<int>("button", 0);
            if (button == 0) {
                // Left click → join channel
                if (!args.empty() && on_join_channel) {
                    on_join_channel(args[0].Get<int>());
                    mobile_show_content = true;
                    dirty("mobile_show_content");
                }
            } else if (button == 1) {
                // Right click → channel context menu
                if (args.size() >= 2 && on_show_channel_menu)
                    on_show_channel_menu(args[0].Get<int>(), args[1].Get<Rml::String>());
            }
        });

    ctor.BindEventCallback("user_mousedown",
        [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
            int button = ev.GetParameter<int>("button", 0);
            if (button != 1) return;  // right-click only
            ev.StopPropagation();     // don't bubble to channel_mousedown
            if (args.size() < 3) return;
            int uid = args[0].Get<int>();
            auto name = args[1].Get<Rml::String>();
            int role = args[2].Get<int>();
            if (on_show_user_menu)
                on_show_user_menu(uid, std::string(name), role);
        });

    // User context menu event callbacks
    ctor.BindEventCallback("close_user_menu",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_user_menu = false;
            dirty("show_user_menu");
        });

    ctor.BindEventCallback("set_user_role",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_set_user_role)
                on_set_user_role(menu_user_id, args[0].Get<int>());
            show_user_menu = false;
            dirty("show_user_menu");
        });

    ctor.BindEventCallback("kick_user",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_kick_user)
                on_kick_user(menu_user_id);
            show_user_menu = false;
            dirty("show_user_menu");
        });

    ctor.BindEventCallback("user_volume_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_user_volume_changed)
                on_user_volume_changed(menu_user_id, menu_user_volume);
        });

    ctor.BindEventCallback("toggle_user_compress",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            menu_user_compress = !menu_user_compress;
            dirty("menu_user_compress");
            if (on_user_compress_changed)
                on_user_compress_changed(menu_user_id, menu_user_compress, menu_user_compress_target);
        });

    ctor.BindEventCallback("user_compress_target_changed",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_user_compress_changed)
                on_user_compress_changed(menu_user_id, menu_user_compress, menu_user_compress_target);
        });

    // Identity backup/import event callbacks
    ctor.BindEventCallback("toggle_seed_phrase",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_show_seed_phrase) on_show_seed_phrase();
        });

    ctor.BindEventCallback("copy_seed_phrase",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_copy_seed_phrase) on_copy_seed_phrase();
        });

    ctor.BindEventCallback("toggle_private_key",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_show_private_key) on_show_private_key();
        });

    ctor.BindEventCallback("copy_private_key",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_copy_private_key) on_copy_private_key();
        });

    ctor.BindEventCallback("show_import_form",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_show_import) on_show_import();
        });

    ctor.BindEventCallback("do_import_identity",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_do_import) on_do_import();
        });

    ctor.BindEventCallback("cancel_import",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_cancel_import) on_cancel_import();
        });

    handle_ = ctor.GetModelHandle();
    return true;
}

void LobbyModel::dirty(const Rml::String& variable) {
    handle_.DirtyVariable(variable);
}

void LobbyModel::dirty_all() {
    dirty("is_connected");
    dirty("connection_insecure");
    dirty("server_name");
    dirty("username");
    dirty("error_text");
    dirty("channels");
    dirty("current_channel");
    dirty("current_channel_name");
    dirty("is_muted");
    dirty("is_deafened");
    dirty("show_settings");
    dirty("capture_devices");
    dirty("playback_devices");
    dirty("selected_capture");
    dirty("selected_playback");
    dirty("denoise_enabled");
    dirty("aec_enabled");
    dirty("normalize_enabled");
    dirty("normalize_target");
    dirty("vad_enabled");
    dirty("vad_threshold");
    dirty("voice_level");
    dirty("notification_volume");
    dirty("ptt_enabled");
    dirty("ptt_key");
    dirty("ptt_key_name");
    dirty("ptt_binding");
    dirty("ptt_delay");
    dirty("mute_key");
    dirty("mute_key_name");
    dirty("mute_binding");
    dirty("deafen_key");
    dirty("deafen_key_name");
    dirty("deafen_binding");
    dirty("mobile_show_content");
    dirty("is_sharing");
    dirty("someone_sharing");
    dirty("sharers");
    dirty("viewing_sharer_id");
    dirty("stream_volume");
    dirty("stream_fullscreen");
    dirty("stream_fps");
    dirty("show_share_picker");
    dirty("use_native_picker");
    dirty("share_targets");
    dirty("share_bitrate");
    dirty("share_fps");
    dirty("share_codec");
    dirty("share_scale");
    dirty("update_available");
    dirty("update_downloading");
    dirty("update_ready");
    dirty("update_version");
    dirty("my_role");
    dirty("can_manage_channels");
    dirty("can_kick");
    dirty("can_manage_roles");
    dirty("show_create_channel");
    dirty("show_rename_channel");
    dirty("rename_channel_name");
    dirty("new_rename_channel_name");
    dirty("admin_message");
    dirty("show_user_menu");
    dirty("menu_user_id");
    dirty("menu_user_name");
    dirty("menu_user_role");
    dirty("menu_user_volume");
    dirty("menu_user_compress");
    dirty("menu_user_compress_target");
    dirty("menu_can_roles");
    dirty("menu_can_kick");
    dirty("show_seed_phrase");
    dirty("identity_seed_phrase");
    dirty("show_import_identity");
    dirty("import_phrase");
    dirty("import_error");
    dirty("show_private_key");
    dirty("identity_private_key");
}

} // namespace parties::client
