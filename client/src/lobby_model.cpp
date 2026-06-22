#include <client/lobby_model.h>

#include <RmlUi/Core/Event.h>

namespace parties::client {

void LobbyModel::build(rml::Builder& b) {
    // Register struct + array types.
    // Order matters: array types must be registered BEFORE structs that contain them as members.
    // ChannelInfo has a Vector<ChannelUser> member, so register ChannelUser + its array first.
    b.register_struct<ChannelUser>([](auto& s) {
        s.member("name",        &ChannelUser::name)
         .member("id",          &ChannelUser::id)
         .member("role",        &ChannelUser::role)
         .member("muted",       &ChannelUser::muted)
         .member("deafened",    &ChannelUser::deafened)
         .member("speaking",    &ChannelUser::speaking)
         .member("streaming",   &ChannelUser::streaming)
         .member("color_index", &ChannelUser::color_index);
    });
    b.register_array<Rml::Vector<ChannelUser>>();

    b.register_struct<ChannelInfo>([](auto& s) {
        s.member("id",         &ChannelInfo::id)
         .member("name",       &ChannelInfo::name)
         .member("user_count", &ChannelInfo::user_count)
         .member("max_users",  &ChannelInfo::max_users)
         .member("users",      &ChannelInfo::users);
    });
    b.register_array<Rml::Vector<ChannelInfo>>();

    b.register_struct<AudioDevice>([](auto& s) {
        s.member("name",  &AudioDevice::name)
         .member("index", &AudioDevice::index);
    });
    b.register_array<Rml::Vector<AudioDevice>>();

    b.register_struct<ShareTarget>([](auto& s) {
        s.member("name",       &ShareTarget::name)
         .member("index",      &ShareTarget::index)
         .member("is_monitor", &ShareTarget::is_monitor);
    });
    b.register_array<Rml::Vector<ShareTarget>>();

    b.register_struct<ActiveSharer>([](auto& s) {
        s.member("id",       &ActiveSharer::id)
         .member("name",     &ActiveSharer::name)
         .member("watching", &ActiveSharer::watching);
    });
    b.register_array<Rml::Vector<ActiveSharer>>();

    b.register_struct<WatchedStream>([](auto& s) {
        s.member("id",         &WatchedStream::id)
         .member("name",       &WatchedStream::name)
         .member("element_id", &WatchedStream::element_id);
    });
    b.register_array<Rml::Vector<WatchedStream>>();

    // Bind variables.
    b.bind("is_connected",      is_connected)
     .bind("ping_ms",           ping_ms)
     .bind("server_name",       server_name)
     .bind("server_initials",   server_initials)
     .bind("server_color_index", server_color_index)
     .bind("username",          username)
     .bind("my_color_index",    my_color_index)
     .bind("error_text",        error_text)
     .bind("channels",          channels)
     .bind("current_channel",       current_channel)
     .bind("current_channel_name",  current_channel_name)
     .bind("is_muted",          is_muted)
     .bind("is_deafened",       is_deafened)
     .bind("show_settings",     show_settings)
     .bind("show_chat",         show_chat)
     .bind("capture_devices",   capture_devices)
     .bind("playback_devices",  playback_devices)
     .bind("selected_capture",  selected_capture)
     .bind("selected_playback", selected_playback)
     .bind("denoise_enabled",   denoise_enabled)
     .bind("normalize_enabled", normalize_enabled)
     .bind("normalize_target",  normalize_target)
     .bind("aec_enabled",       aec_enabled)
     .bind("vad_enabled",       vad_enabled)
     .bind("vad_threshold",     vad_threshold)
     .bind("voice_level",       voice_level)
     .bind("voice_volume",      voice_volume)
     .bind("secondary_volume",  secondary_volume)
     .bind("notification_volume", notification_volume)
     .bind("ptt_enabled",       ptt_enabled)
     .bind("ptt_key",           ptt_key)
     .bind("ptt_key_name",      ptt_key_name)
     .bind("ptt_binding",       ptt_binding)
     .bind("ptt_delay",         ptt_delay)
     .bind("mute_key",          mute_key)
     .bind("mute_key_name",     mute_key_name)
     .bind("mute_binding",      mute_binding)
     .bind("deafen_key",        deafen_key)
     .bind("deafen_key_name",   deafen_key_name)
     .bind("deafen_binding",    deafen_binding)
     .bind("mobile_show_content", mobile_show_content)
     .bind("is_sharing",        is_sharing)
     .bind("someone_sharing",   someone_sharing)
     .bind("sharers",           sharers)
     .bind("watched",           watched)
     .bind("watching_count",    watching_count)
     .bind("viewing_sharer_id", viewing_sharer_id)
     .bind("stream_volume",     stream_volume)
     .bind("stream_fullscreen", stream_fullscreen)
     .bind("stream_fps",        stream_fps)
     .bind("show_share_picker", show_share_picker)
     .bind("use_native_picker", use_native_picker)
     .bind("share_targets",     share_targets)
     .bind("share_bitrate",     share_bitrate)
     .bind("share_fps",         share_fps)
     .bind("share_codec",       share_codec)
     .bind("share_scale",       share_scale)
     .bind("update_available",   update_available)
     .bind("update_downloading", update_downloading)
     .bind("update_ready",       update_ready)
     .bind("update_version",     update_version)
     .bind("my_role",              my_role)
     .bind("can_manage_channels",  can_manage_channels)
     .bind("can_kick",             can_kick)
     .bind("can_manage_roles",     can_manage_roles)
     .bind("show_create_channel",  show_create_channel)
     .bind("new_channel_name",     new_channel_name)
     .bind("show_rename_channel",      show_rename_channel)
     .bind("rename_channel_id",        rename_channel_id)
     .bind("rename_channel_name",      rename_channel_name)
     .bind("new_rename_channel_name",  new_rename_channel_name)
     .bind("admin_message",        admin_message)
     .bind("show_user_menu",     show_user_menu)
     .bind("menu_user_id",       menu_user_id)
     .bind("menu_user_name",     menu_user_name)
     .bind("menu_user_role",     menu_user_role)
     .bind("menu_user_volume",   menu_user_volume)
     .bind("menu_user_compress", menu_user_compress)
     .bind("menu_user_compress_target", menu_user_compress_target)
     .bind("menu_can_roles",     menu_can_roles)
     .bind("menu_can_kick",      menu_can_kick)
     .bind("show_seed_phrase",       show_seed_phrase)
     .bind("identity_seed_phrase",   identity_seed_phrase)
     .bind("show_import_identity",   show_import_identity)
     .bind("import_phrase",          import_phrase)
     .bind("import_error",           import_error)
     .bind("show_private_key",       show_private_key)
     .bind("identity_private_key",   identity_private_key);

    // Event callbacks. Property assignments inside these lambdas auto-dirty.
    b.on("apply_update", [this] {
        if (on_apply_update) on_apply_update();
    });

    b.on("disconnect_server", [this] {
        if (on_disconnect_server) on_disconnect_server();
    });

    b.on_args<int>("join_channel", [this](int id) {
        if (on_join_channel) {
            on_join_channel(id);
            mobile_show_content = true;
        }
    });

    b.on("leave_channel", [this] {
        if (on_leave_channel) on_leave_channel();
        mobile_show_content = false;
    });

    b.on("toggle_mute", [this] {
        if (on_toggle_mute) on_toggle_mute();
    });

    b.on("toggle_deafen", [this] {
        if (on_toggle_deafen) on_toggle_deafen();
    });

    b.on("toggle_settings", [this] {
        show_settings = !show_settings.get();
        mobile_show_content = show_settings.get();
    });

    b.on_args<int>("select_capture", [this](int idx) {
        selected_capture = idx;
        if (on_select_capture) on_select_capture(idx);
    });

    b.on_args<int>("select_playback", [this](int idx) {
        selected_playback = idx;
        if (on_select_playback) on_select_playback(idx);
    });

    b.on("toggle_denoise", [this] {
        denoise_enabled = !denoise_enabled.get();
        if (on_denoise_changed) on_denoise_changed(denoise_enabled.get());
    });

    b.on("toggle_aec", [this] {
        aec_enabled = !aec_enabled.get();
        if (on_aec_changed) on_aec_changed(aec_enabled.get());
    });

    b.on("toggle_normalize", [this] {
        normalize_enabled = !normalize_enabled.get();
        if (on_normalize_changed) on_normalize_changed(normalize_enabled.get());
    });

    b.on("toggle_vad", [this] {
        vad_enabled = !vad_enabled.get();
        if (on_vad_changed) on_vad_changed(vad_enabled.get());
    });

    b.on("normalize_target_changed", [this] {
        if (on_normalize_target_changed) on_normalize_target_changed(normalize_target.get());
    });

    b.on("vad_threshold_changed", [this] {
        if (on_vad_threshold_changed) on_vad_threshold_changed(vad_threshold.get());
    });

    b.on("notification_volume_changed", [this] {
        if (on_notification_volume_changed) on_notification_volume_changed(notification_volume.get());
    });

    b.on("voice_volume_changed", [this] {
        if (on_voice_volume_changed) on_voice_volume_changed(voice_volume.get());
    });

    b.on("secondary_volume_changed", [this] {
        if (on_secondary_volume_changed) on_secondary_volume_changed(secondary_volume.get());
    });

    b.on("test_notification_sound", [this] {
        if (on_test_notification_sound) on_test_notification_sound();
    });

    b.on("toggle_ptt", [this] {
        if (on_toggle_ptt) on_toggle_ptt();
    });

    b.on("ptt_bind", [this] {
        if (on_ptt_bind) on_ptt_bind();
    });

    b.on("mute_bind", [this] {
        if (on_mute_bind) on_mute_bind();
    });

    b.on("deafen_bind", [this] {
        if (on_deafen_bind) on_deafen_bind();
    });

    b.on("ptt_delay_changed", [this] {
        if (on_ptt_delay_changed) on_ptt_delay_changed(ptt_delay.get());
    });

    b.on("toggle_share", [this] {
        if (on_toggle_share) on_toggle_share();
    });

    b.on_args<int>("select_share_target", [this](int idx) {
        if (on_select_share_target) on_select_share_target(idx);
    });

    b.on("cancel_share", [this] {
        if (on_cancel_share) on_cancel_share();
    });

    b.on("start_native_share", [this] {
        if (on_start_native_share) on_start_native_share();
    });

    b.on_args<int>("select_share_fps", [this](int v) {
        share_fps = v;
    });

    b.on("share_bitrate_changed", [this] {
        if (on_share_bitrate_changed) on_share_bitrate_changed(share_bitrate.get());
    });

    b.on_args<int>("select_share_codec", [this](int v) {
        share_codec = v;
    });

    b.on_args<int>("select_share_scale", [this](int v) {
        share_scale = v;
    });

    b.on_args<int>("watch_sharer", [this](int id) {
        if (on_watch_sharer) {
            on_watch_sharer(id);
            mobile_show_content = true;
        }
    });

    b.on_args<int>("select_sharer", [this](int id) {
        if (on_select_sharer) on_select_sharer(id);
    });

    b.on_args<int>("toggle_watch", [this](int id) {
        if (on_toggle_watch) on_toggle_watch(id);
        mobile_show_content = true;
    });

    b.on("stop_watching", [this] {
        if (on_stop_watching) on_stop_watching();
        mobile_show_content = false;
    });

    b.on("mobile_back", [this] {
        mobile_show_content = false;
        show_settings = false;
        if (on_stop_watching) on_stop_watching();
    });

    b.on("stream_volume_changed", [this] {
        if (on_stream_volume_changed) on_stream_volume_changed(stream_volume.get());
    });

    b.on("toggle_stream_fullscreen", [this] {
        stream_fullscreen = !stream_fullscreen.get();
    });

    b.on("stream_tap_fullscreen", [this] {
        if (on_stream_tap_fullscreen) on_stream_tap_fullscreen();
    });

    // Admin event callbacks
    b.on("show_create_channel_form", [this] {
        show_create_channel = true;
        new_channel_name = "";
    });

    b.on("cancel_create_channel", [this] {
        show_create_channel = false;
    });

    b.on("create_channel", [this] {
        if (on_create_channel) on_create_channel();
    });

    b.on("cancel_rename_channel", [this] {
        show_rename_channel = false;
    });

    b.on("rename_channel", [this] {
        if (on_rename_channel) on_rename_channel();
    });

    b.on_event("channel_mousedown", [this](Rml::Event& ev, const Rml::VariantList& args) {
        int button = ev.GetParameter<int>("button", 0);
        if (button == 0) {
            // Left click → join channel
            if (!args.empty() && on_join_channel) {
                on_join_channel(args[0].Get<int>());
                mobile_show_content = true;
            }
        } else if (button == 1) {
            // Right click → channel context menu
            if (args.size() >= 2 && on_show_channel_menu)
                on_show_channel_menu(args[0].Get<int>(), std::string(args[1].Get<Rml::String>()));
        }
    });

    b.on_event("user_mousedown", [this](Rml::Event& ev, const Rml::VariantList& args) {
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
    b.on("close_user_menu", [this] {
        show_user_menu = false;
    });

    b.on_args<int>("set_user_role", [this](int role) {
        if (on_set_user_role) on_set_user_role(menu_user_id.get(), role);
        show_user_menu = false;
    });

    b.on("kick_user", [this] {
        if (on_kick_user) on_kick_user(menu_user_id.get());
        show_user_menu = false;
    });

    b.on("user_volume_changed", [this] {
        if (on_user_volume_changed)
            on_user_volume_changed(menu_user_id.get(), menu_user_volume.get());
    });

    b.on("toggle_user_compress", [this] {
        menu_user_compress = !menu_user_compress.get();
        if (on_user_compress_changed)
            on_user_compress_changed(menu_user_id.get(), menu_user_compress.get(),
                                     menu_user_compress_target.get());
    });

    b.on("user_compress_target_changed", [this] {
        if (on_user_compress_changed)
            on_user_compress_changed(menu_user_id.get(), menu_user_compress.get(),
                                     menu_user_compress_target.get());
    });

    // Identity backup/import event callbacks
    b.on("toggle_seed_phrase", [this] {
        if (on_show_seed_phrase) on_show_seed_phrase();
    });

    b.on("copy_seed_phrase", [this] {
        if (on_copy_seed_phrase) on_copy_seed_phrase();
    });

    b.on("toggle_private_key", [this] {
        if (on_show_private_key) on_show_private_key();
    });

    b.on("copy_private_key", [this] {
        if (on_copy_private_key) on_copy_private_key();
    });

    b.on("show_import_form", [this] {
        if (on_show_import) on_show_import();
    });

    b.on("do_import_identity", [this] {
        if (on_do_import) on_do_import();
    });

    b.on("cancel_import", [this] {
        if (on_cancel_import) on_cancel_import();
    });
}

} // namespace parties::client
