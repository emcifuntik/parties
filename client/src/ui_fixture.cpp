#include <client/ui_fixture.h>

#include <client/app_core.h>
#include <client/video_element.h>

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace parties::client {
namespace {

ChannelUser FixtureUser(const char* name, int id, bool speaking = false,
                        bool muted = false, bool streaming = false, int role = 3)
{
    ChannelUser user;
    user.name = name;
    user.id = id;
    user.role = role;
    user.speaking = speaking;
    user.muted = muted;
    user.streaming = streaming;
    user.color_index = id % 12;
    return user;
}

ChatMessage FixtureMessage(int64_t id, int sender_id, const char* sender,
                           const char* text, const char* time, int color,
                           bool own = false, bool pinned = false)
{
    ChatMessage message;
    message.id = id;
    message.sender_id = sender_id;
    message.sender_name = sender;
    message.initials = Rml::String(sender).substr(0, (std::min)(size_t(2), std::strlen(sender)));
    message.text = text;
    message.timestamp_str = time;
    message.color_index = color;
    message.is_own = own;
    message.pinned = pinned;
    message.segments.push_back({text, false});
    return message;
}

bool StartsWith(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::vector<uint8_t> MockStreamFrame(int variant)
{
    constexpr int width = 640;
    constexpr int height = 360;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const std::array<uint8_t, 3> top = variant == 0
        ? std::array<uint8_t, 3>{25, 42, 61}
        : std::array<uint8_t, 3>{48, 32, 58};
    const std::array<uint8_t, 3> bottom = variant == 0
        ? std::array<uint8_t, 3>{20, 116, 105}
        : std::array<uint8_t, 3>{69, 69, 154};
    for (int y = 0; y < height; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(height - 1);
        for (int x = 0; x < width; ++x) {
            const float glow = (x > width / 5 && x < width * 4 / 5 &&
                                y > height / 5 && y < height * 4 / 5) ? 18.0f : 0.0f;
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            pixels[offset + 0] = static_cast<uint8_t>((1.0f - t) * top[0] + t * bottom[0] + glow);
            pixels[offset + 1] = static_cast<uint8_t>((1.0f - t) * top[1] + t * bottom[1] + glow);
            pixels[offset + 2] = static_cast<uint8_t>((1.0f - t) * top[2] + t * bottom[2] + glow);
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

} // namespace

bool IsUIFixtureScenario(const std::string& scenario)
{
    static constexpr const char* scenarios[] = {
        "launcher", "launcher-reconnecting", "party-modal", "update-available",
        "onboarding", "onboarding-restore", "onboarding-key-import", "recovery",
        "login", "login-existing", "tofu", "global-name", "server-nickname",
        "room", "room-empty", "chat", "chat-search", "chat-pinned", "chat-attachment",
        "settings", "settings-select-open", "settings-screen-share", "settings-hotkeys",
        "settings-account", "settings-account-import", "share", "audio-share",
        "stream-single", "streams", "member", "create-channel", "create-text-channel",
        "rename-channel", "native-user", "native-channel", "native-server",
        "native-message", "native-share-picker", "native-audio-picker"
    };
    for (const char* value : scenarios)
        if (scenario == value) return true;
    return false;
}

void PopulateUIFixture(AppCore& core, const std::string& scenario, bool macos)
{
    LobbyModel& lobby = core.model_;
    ServerListModel& servers = core.server_model_;
    ChatModel& chat = core.chat_model_;

    const bool disconnected = scenario == "launcher" || scenario == "launcher-reconnecting" ||
        scenario == "party-modal" || scenario == "update-available" ||
        StartsWith(scenario, "onboarding") || scenario == "recovery" ||
        StartsWith(scenario, "login") || scenario == "tofu" ||
        scenario == "global-name" || scenario == "server-nickname" ||
        scenario == "native-server";

    lobby.is_connected = !disconnected;
    lobby.server_name = "Night Shift";
    lobby.server_initials = "NS";
    lobby.server_color_index = 1;
    lobby.username = "tuxick";
    lobby.my_color_index = 4;
    lobby.current_channel = scenario == "room-empty" ? 0 : 1;
    lobby.current_channel_name = "General";
    lobby.ping_ms = 24;
    lobby.can_manage_channels = true;
    lobby.can_manage_roles = true;
    lobby.can_kick = true;
    lobby.my_role = 0;
    lobby.voice_volume = 1.0f;
    lobby.secondary_volume = 0.82f;
    lobby.music_send_volume = 0.72f;
    lobby.notification_volume = 0.75f;
    lobby.denoise_enabled = true;
    lobby.normalize_enabled = true;
    lobby.normalize_target = 0.55f;
    lobby.aec_enabled = true;
    lobby.vad_enabled = true;
    lobby.vad_threshold = 0.36f;
    lobby.voice_level = 0.48f;
    lobby.ptt_enabled = true;
    lobby.ptt_delay = 250.0f;
    lobby.ptt_delay_text = "250 ms";
    lobby.share_bitrate = 8.0f;
    lobby.share_fps = 2;
    lobby.share_codec = 1;
    lobby.share_scale = 0;
    lobby.mobile_show_content = !disconnected;

    ChannelInfo general;
    general.id = 1;
    general.name = "General";
    general.max_users = 64;
    general.users = {
        FixtureUser("tuxick", 1, false, false, false, 0),
        FixtureUser("IceTroll", 2, true, false, false, 1),
        FixtureUser("ivan", 3, false, false, true),
        FixtureUser("Sara", 4), FixtureUser("Maks", 5, false, true),
        FixtureUser("android", 6), FixtureUser("Noah", 7)
    };
    general.user_count = static_cast<int>(general.users.size());
    ChannelInfo lounge;
    lounge.id = 2;
    lounge.name = "Late night";
    lounge.max_users = 12;
    lounge.users = {FixtureUser("Maya", 8), FixtureUser("Liam", 9, true)};
    lounge.user_count = static_cast<int>(lounge.users.size());
    ChannelInfo focus;
    focus.id = 3;
    focus.name = "Quiet focus";
    focus.max_users = 8;
    lobby.channels = Rml::Vector<ChannelInfo>{general, lounge, focus};

    lobby.capture_devices = Rml::Vector<AudioDevice>{{macos ? "MacBook Pro Microphone" : "iPhone Microphone", 0},
                                                      {"External USB Microphone", 1}};
    lobby.playback_devices = Rml::Vector<AudioDevice>{{macos ? "MacBook Pro Speakers" : "iPhone Speaker", 0},
                                                       {"AirPods Pro", 1}};
    lobby.selected_capture = 0;
    lobby.selected_playback = 0;
    lobby.mute_key = macos ? 1 : 0;
    lobby.deafen_key = macos ? 1 : 0;
    lobby.ptt_key = macos ? 1 : 0;
    lobby.mute_key_name = macos ? "⌘⇧M" : "Not available on iOS";
    lobby.deafen_key_name = macos ? "⌘⇧D" : "Not available on iOS";
    lobby.ptt_key_name = macos ? "Space" : "Not available on iOS";

    ServerEntry night;
    night.id = 1; night.name = "Night Shift"; night.initials = "NS";
    night.color_index = 1; night.host = "night.parties.local";
    night.online = true; night.users_text = "12 online · 5 in General";
    ServerEntry studio;
    studio.id = 2; studio.name = "Creative Studio"; studio.initials = "CS";
    studio.color_index = 2; studio.host = "studio.parties.local";
    studio.online = true; studio.users_text = "7 online · 3 in Lounge";
    ServerEntry guild;
    guild.id = 3; guild.name = "Old Guild"; guild.initials = "OG";
    guild.color_index = 3; guild.host = "guild.parties.local";
    guild.online = false; guild.locked = true;
    servers.servers = Rml::Vector<ServerEntry>{night, studio, guild};
    servers.party_count_text = "3 parties";
    servers.global_name = "tuxick";
    servers.connected_server_id = disconnected ? 0 : 1;
    servers.fingerprint = "5E7A 91C2 4D3F";
    servers.has_identity = true;
    servers.seed_phrase = "amber vessel orbit meadow copper velvet island echo marble lunar gentle harbor";

    ChatMessage pinned = FixtureMessage(1, 2, "IceTroll",
        "Anyone up for a quick match later?", "20:41", 2, false, true);
    ChatMessage second = FixtureMessage(2, 3, "ivan",
        "I can join after I finish this build.", "20:43", 5);
    ChatMessage third = FixtureMessage(3, 4, "Sara",
        "Perfect. I pinned the server details above.", "20:44", 8);
    ChatMessage own = FixtureMessage(4, 1, "tuxick",
        "Give me ten minutes and I'll be there.", "20:45", 3, true);
    if (scenario == "chat-attachment") {
        third.attachments.push_back({42, "match-plan.pdf", "2.4 MB", "PDF", true});
        chat.pending_files = Rml::Vector<PendingFile>{{"screenshot.png", "684 KB", "/tmp/screenshot.png"}};
    }

    chat.text_channels = Rml::Vector<TextChannel>{
        {1, "general", false}, {2, "clips-and-links", true}, {3, "off-topic", false}};
    const bool chat_route = StartsWith(scenario, "chat") || scenario == "native-message";
    chat.active_channel = chat_route ? 1 : 0;
    chat.active_channel_name = "general";
    chat.can_manage_channels = true;
    chat.messages = Rml::Vector<ChatMessage>{pinned, second, third, own};
    chat.has_more_history = scenario == "chat-attachment";
    chat.show_search = scenario == "chat-search";
    chat.search_query = "server";
    chat.search_results = Rml::Vector<ChatMessage>{third};
    chat.show_pinned = scenario == "chat-pinned";
    chat.pinned_messages = Rml::Vector<ChatMessage>{pinned};

    lobby.router.reset();
    if (chat_route) {
        lobby.router.go(DocumentRoute::Chat);
    } else if (StartsWith(scenario, "settings")) {
        lobby.router.go(DocumentRoute::Settings);
        SettingsSection section = SettingsSection::AudioVoice;
        if (scenario == "settings-screen-share") section = SettingsSection::ScreenShare;
        if (scenario == "settings-hotkeys") section = SettingsSection::Hotkeys;
        if (scenario == "settings-account" || scenario == "settings-account-import")
            section = SettingsSection::AccountKeys;
        lobby.router.select_settings(section);
    } else if (scenario == "share" || scenario == "audio-share" ||
               scenario == "native-share-picker" || scenario == "native-audio-picker") {
        lobby.router.go(DocumentRoute::SharePicker);
        lobby.share_picker_mode = (scenario == "audio-share" || scenario == "native-audio-picker") ? 1 : 0;
        lobby.use_native_picker = macos;
        lobby.share_monitor_targets = Rml::Vector<ShareTarget>{{"Studio Display", 0, true, "share-target-0"}};
        lobby.share_application_targets = Rml::Vector<ShareTarget>{
            {"Zen Browser", 1, false, "share-target-1"},
            {"Visual Studio Code", 2, false, "share-target-2"}};
        lobby.selected_share_target = macos ? -1 : 0;
    } else if (scenario == "stream-single" || scenario == "streams") {
        lobby.router.go(DocumentRoute::Streams);
        lobby.someone_sharing = true;
        lobby.watching_count = scenario == "stream-single" ? 1 : 2;
        lobby.viewing_sharer_id = 2;
        lobby.stream_volume = 0.76f;
        lobby.stream_fps = 60;
        lobby.sharers = Rml::Vector<ActiveSharer>{{2, "IceTroll", true}, {3, "ivan", true}};
        lobby.watched = Rml::Vector<WatchedStream>{{2, "IceTroll · Counter-Strike 2", "screen-share-2"}};
        if (scenario == "streams")
            lobby.watched.silent().push_back({3, "ivan · Zen Browser", "screen-share-3"});
    }

    servers.reconnecting = scenario == "launcher-reconnecting";
    servers.reconnect_status = "Reconnecting to Night Shift…";
    servers.show_onboarding = StartsWith(scenario, "onboarding") || scenario == "recovery";
    servers.onboarding_step = scenario == "recovery" ? 1 : 0;
    servers.show_restore = scenario == "onboarding-restore";
    servers.show_key_import = scenario == "onboarding-key-import";
    if (servers.show_onboarding.get()) servers.has_identity = false;
    servers.show_add_form = scenario == "party-modal";
    servers.edit_host = "voice.example.com";
    servers.edit_port = "7800";
    servers.edit_nickname = "";
    servers.show_login = StartsWith(scenario, "login");
    servers.login_status = "Secure connection ready";
    servers.login_show_username = scenario == "login";
    servers.login_username = "tuxick";
    servers.show_global_name_editor = scenario == "global-name";
    servers.global_name_input = "tuxick";
    servers.show_server_nickname_editor = scenario == "server-nickname";
    servers.server_nickname_server_id = 1;
    servers.server_nickname_server_name = "Night Shift";
    servers.server_nickname_input = "tuxick@night";
    servers.show_tofu_warning = scenario == "tofu";
    servers.tofu_fingerprint = "71:08:BB:6E:20:91:4F:3A";

    lobby.update_available = scenario == "update-available";
    lobby.update_version = "0.8.0";
    lobby.show_create_channel = scenario == "create-channel";
    lobby.new_channel_name = "Strategy";
    lobby.show_rename_channel = scenario == "rename-channel";
    lobby.rename_channel_id = 1;
    lobby.rename_channel_name = "General";
    lobby.new_rename_channel_name = "General lounge";
    chat.show_create_text_channel = scenario == "create-text-channel";
    chat.new_text_channel_name = "release-notes";

    lobby.show_user_menu = scenario == "member";
    if (scenario == "member") {
        lobby.menu_user_id = 2;
        lobby.menu_user_name = "IceTroll";
        lobby.menu_user_role = 1;
        lobby.menu_user_volume = 0.86f;
        lobby.menu_user_music_volume = 0.64f;
        lobby.menu_user_compress = true;
        lobby.menu_user_compress_target = 0.55f;
        lobby.menu_can_roles = true;
        lobby.menu_can_kick = true;
    }
    if (scenario == "room" || scenario == "native-user" || scenario == "native-channel") {
        lobby.someone_sharing = true;
        lobby.sharers = Rml::Vector<ActiveSharer>{{3, "ivan", false}};
    }
    if (scenario == "settings-account" || scenario == "settings-account-import") {
        lobby.show_private_key = true;
        lobby.identity_private_key = "8d99c2eed598508a94eb471d7334ee80d28a57139d1a7b7273e16105106fe0a4";
        lobby.show_import_identity = scenario == "settings-account-import";
    }
}

void ApplyUIFixtureDocument(Rml::ElementDocument* document, const std::string& scenario)
{
    if (!document) return;
    document->GetContext()->Update();

    if (scenario == "settings-select-open") {
        if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(
                document->GetElementById("settings-capture-select")))
            select->ShowSelectBox();
    }

    if (scenario == "stream-single" || scenario == "streams") {
        const int frame_count = scenario == "stream-single" ? 1 : 2;
        for (int index = 0; index < frame_count; ++index) {
            const Rml::String id = index == 0 ? "screen-share-2" : "screen-share-3";
            if (auto* video = rmlui_dynamic_cast<VideoElement*>(document->GetElementById(id)))
                video->UpdateFrame(MockStreamFrame(index), 640, 360);
        }
    }
}

} // namespace parties::client
