#include <client/app.h>
#include <client/context_menu.h>
#include <client/screen_capture.h>
#include <client/video_encoder.h>
#include <client/video_decoder.h>
#include <client/video_element.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>

#include <RmlUi/Core/Factory.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace parties::client {

static std::string vk_to_name(int vk) {
    switch (vk) {
    case VK_LBUTTON:  return "Mouse1";
    case VK_RBUTTON:  return "Mouse2";
    case VK_MBUTTON:  return "Mouse3";
    case VK_XBUTTON1: return "Mouse4";
    case VK_XBUTTON2: return "Mouse5";
    case VK_SPACE:    return "Space";
    case VK_RETURN:   return "Enter";
    case VK_TAB:      return "Tab";
    case VK_BACK:     return "Backspace";
    case VK_SHIFT:    return "Shift";
    case VK_CONTROL:  return "Ctrl";
    case VK_MENU:     return "Alt";
    case VK_CAPITAL:  return "CapsLock";
    case VK_LSHIFT:   return "LShift";
    case VK_RSHIFT:   return "RShift";
    case VK_LCONTROL: return "LCtrl";
    case VK_RCONTROL: return "RCtrl";
    case VK_LMENU:    return "LAlt";
    case VK_RMENU:    return "RAlt";
    default: {
        UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
        if (ch > 0 && ch < 128)
            return std::string(1, static_cast<char>(std::toupper(ch)));
        return "Key " + std::to_string(vk);
    }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// App implementation
// ═══════════════════════════════════════════════════════════════════════

App::App() = default;
App::~App() { shutdown(); }

void App::setup_model_callbacks() {
    model_.on_join_channel = [this](int id) {
        join_channel(static_cast<ChannelId>(id));
    };

    model_.on_leave_channel = [this]() {
        leave_channel();
    };

    model_.on_toggle_mute = [this]() {
        if (model_.ptt_enabled) return;  // PTT controls mute state
        bool muted = !audio_.is_muted();
        audio_.set_muted(muted);
        model_.is_muted = muted;
        model_.dirty("is_muted");
        sound_player_.play(muted ? SoundPlayer::Effect::Mute
                                 : SoundPlayer::Effect::Unmute);
    };

    model_.on_toggle_deafen = [this]() {
        bool deafened = !audio_.is_deafened();
        audio_.set_deafened(deafened);
        model_.is_deafened = deafened;
        model_.dirty("is_deafened");
        sound_player_.play(deafened ? SoundPlayer::Effect::Deafen
                                    : SoundPlayer::Effect::Undeafen);
    };

    model_.on_select_capture = [this](int index) {
        audio_.set_capture_device(index);
        auto devs = audio_.get_capture_devices();
        if (index >= 0 && index < static_cast<int>(devs.size()))
            settings_.set_pref("audio.capture_device", devs[index].name);
    };

    model_.on_select_playback = [this](int index) {
        audio_.set_playback_device(index);
        auto devs = audio_.get_playback_devices();
        if (index >= 0 && index < static_cast<int>(devs.size()))
            settings_.set_pref("audio.playback_device", devs[index].name);
    };

    model_.on_denoise_changed = [this](bool enabled) {
        audio_.set_denoise_enabled(enabled);
        settings_.set_pref("audio.denoise", enabled ? "1" : "0");
    };

    model_.on_normalize_changed = [this](bool enabled) {
        audio_.set_normalize_enabled(enabled);
        settings_.set_pref("audio.normalize", enabled ? "1" : "0");
    };

    model_.on_normalize_target_changed = [this](float target) {
        audio_.set_normalize_target(target);
        settings_.set_pref("audio.normalize_target", std::to_string(target));
    };

    model_.on_vad_changed = [this](bool enabled) {
        audio_.set_vad_enabled(enabled);
        settings_.set_pref("audio.vad", enabled ? "1" : "0");
    };

    model_.on_vad_threshold_changed = [this](float threshold) {
        audio_.set_vad_threshold(threshold);
        settings_.set_pref("audio.vad_threshold", std::to_string(threshold));
    };

    model_.on_toggle_ptt = [this]() {
        model_.ptt_enabled = !model_.ptt_enabled;
        model_.dirty("ptt_enabled");
        settings_.set_pref("audio.ptt", model_.ptt_enabled ? "1" : "0");
        if (model_.ptt_enabled) {
            // PTT starts muted
            audio_.set_muted(true);
            model_.is_muted = true;
            model_.dirty("is_muted");
        }
    };

    model_.on_ptt_bind = [this]() {
        model_.ptt_binding = true;
        model_.dirty("ptt_binding");
    };

    model_.on_ptt_delay_changed = [this](float delay) {
        settings_.set_pref("audio.ptt_delay", std::to_string(static_cast<int>(delay)));
    };

    model_.on_toggle_share = [this]() {
        if (sharing_screen_)
            stop_screen_share();
        else
            show_share_picker();
    };

    model_.on_select_share_target = [this](int index) {
        model_.show_share_picker = false;
        model_.dirty("show_share_picker");
        start_screen_share(index);
    };

    model_.on_cancel_share = [this]() {
        model_.show_share_picker = false;
        model_.dirty("show_share_picker");
        capture_targets_.clear();
    };

    model_.on_watch_sharer = [this](int id) {
        watch_sharer(static_cast<UserId>(id));
    };

    model_.on_select_sharer = [this](int id) {
        watch_sharer(static_cast<UserId>(id));
    };

    model_.on_stop_watching = [this]() {
        stop_watching();
    };

    // Admin operations
    model_.on_create_channel = [this]() {
        if (!authenticated_) return;
        std::string name(model_.new_channel_name);
        if (name.empty()) return;

        BinaryWriter writer;
        writer.write_string(name);
        writer.write_u32(0);  // max_users = 0 (server default)
        net_.send_message(protocol::ControlMessageType::ADMIN_CREATE_CHANNEL,
                          writer.data().data(), writer.data().size());

        model_.show_create_channel = false;
        model_.dirty("show_create_channel");
    };

    model_.on_delete_channel = [this](int channel_id) {
        if (!authenticated_) return;

        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(channel_id));
        net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                          writer.data().data(), writer.data().size());
    };

    model_.on_show_user_menu = [this](int user_id, std::string name, int user_role) {
        if (!authenticated_) return;

        constexpr int ID_SET_ADMIN = 2;
        constexpr int ID_SET_MOD   = 3;
        constexpr int ID_SET_USER  = 4;
        constexpr int ID_KICK      = 10;

        bool can_roles = model_.can_manage_roles && role_ <= user_role;
        bool can_kick_user = model_.can_kick && role_ <= user_role;
        if (!can_roles && !can_kick_user) return;

        std::vector<ContextMenu::Item> items;
        if (can_roles) {
            // Owner (role 0) is set only in server config — not assignable here
            if (user_role != 1) items.push_back({L"Set Admin",     ID_SET_ADMIN});
            if (user_role != 2) items.push_back({L"Set Moderator", ID_SET_MOD});
            if (user_role != 3) items.push_back({L"Set User",      ID_SET_USER});
        }
        if (can_kick_user) {
            if (can_roles) items.push_back({L"", 0, false, true}); // separator
            items.push_back({L"Kick", ID_KICK, true});
        }

        int cmd = ContextMenu::show(hwnd_, items);
        if (cmd == 0) return;

        if (cmd == ID_KICK) {
            BinaryWriter writer;
            writer.write_u32(static_cast<uint32_t>(user_id));
            net_.send_message(protocol::ControlMessageType::ADMIN_KICK_USER,
                              writer.data().data(), writer.data().size());
        } else {
            int new_role = -1;
            switch (cmd) {
            case ID_SET_ADMIN: new_role = 1; break;
            case ID_SET_MOD:   new_role = 2; break;
            case ID_SET_USER:  new_role = 3; break;
            }
            if (new_role >= 0) {
                BinaryWriter writer;
                writer.write_u32(static_cast<uint32_t>(user_id));
                writer.write_u8(static_cast<uint8_t>(new_role));
                net_.send_message(protocol::ControlMessageType::ADMIN_SET_ROLE,
                                  writer.data().data(), writer.data().size());
            }
        }
    };

    model_.on_show_channel_menu = [this](int channel_id, std::string channel_name) {
        if (!authenticated_ || !model_.can_manage_channels) return;

        constexpr int ID_DELETE = 1;
        std::vector<ContextMenu::Item> items;
        items.push_back({L"Delete Channel", ID_DELETE, true});

        int cmd = ContextMenu::show(hwnd_, items);
        if (cmd == ID_DELETE) {
            BinaryWriter writer;
            writer.write_u32(static_cast<uint32_t>(channel_id));
            net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                              writer.data().data(), writer.data().size());
        }
    };
}

void App::setup_server_model_callbacks() {
    server_model_.on_connect_server = [this](int id) {
        // Already connected to this server — do nothing
        if (authenticated_ && id == server_model_.connected_server_id)
            return;

        // Login popup already visible — don't show it again
        if (server_model_.show_login)
            return;

        // If no identity, show onboarding instead
        if (!has_identity_) {
            server_model_.show_onboarding = true;
            server_model_.dirty("show_onboarding");
            return;
        }

        // Find the server entry and show login overlay
        auto saved = settings_.get_saved_servers();
        for (auto& srv : saved) {
            if (srv.id == id) {
                connecting_server_id_ = id;
                server_host_ = srv.host;
                server_port_ = static_cast<uint16_t>(srv.port);

                server_model_.login_username = Rml::String(srv.last_username);
                server_model_.login_error = "";
                server_model_.login_status = Rml::String(srv.name + " - " + srv.host + ":" + std::to_string(srv.port));
                server_model_.show_login = true;

                server_model_.dirty("login_username");
                server_model_.dirty("login_error");
                server_model_.dirty("login_status");
                server_model_.dirty("show_login");
                break;
            }
        }
    };

    server_model_.on_delete_server = [this](int id) {
        settings_.delete_server(id);
        refresh_server_list();
    };

    server_model_.on_save_server = [this]() {
        // Validate
        auto& name = server_model_.edit_name;
        auto& host = server_model_.edit_host;
        auto& port_str = server_model_.edit_port;

        if (name.empty() || host.empty() || port_str.empty()) {
            server_model_.edit_error = "Please fill in all fields";
            server_model_.dirty("edit_error");
            return;
        }

        int port = std::atoi(port_str.c_str());
        if (port <= 0 || port > 65535) {
            server_model_.edit_error = "Invalid port number";
            server_model_.dirty("edit_error");
            return;
        }

        // If editing, delete the old entry first (handles host/port changes)
        if (server_model_.editing_id > 0) {
            settings_.delete_server(server_model_.editing_id);
        }

        settings_.save_server(std::string(name), std::string(host), port, "", "");

        server_model_.show_add_form = false;
        server_model_.dirty("show_add_form");
        refresh_server_list();
    };

    server_model_.on_do_connect = [this]() {
        do_connect();
    };

    server_model_.on_cancel_login = [this]() {
        // Disconnect if we were mid-connection
        if (net_.is_connected()) {
            net_.disconnect();
        }
    };

    server_model_.on_show_server_menu = [this](int id) {
        constexpr int ID_EDIT   = 1;
        constexpr int ID_DELETE = 2;
        std::vector<ContextMenu::Item> items;
        items.push_back({L"Edit",   ID_EDIT});
        items.push_back({L"Delete", ID_DELETE, true});

        int cmd = ContextMenu::show(hwnd_, items);

        if (cmd == ID_EDIT) {
            // Populate edit form
            auto saved = settings_.get_saved_servers();
            for (auto& srv : saved) {
                if (srv.id == id) {
                    server_model_.editing_id = id;
                    server_model_.edit_name = Rml::String(srv.name);
                    server_model_.edit_host = Rml::String(srv.host);
                    server_model_.edit_port = Rml::String(std::to_string(srv.port));
                    server_model_.edit_error = "";
                    server_model_.show_add_form = true;
                    server_model_.dirty("editing_id");
                    server_model_.dirty("edit_name");
                    server_model_.dirty("edit_host");
                    server_model_.dirty("edit_port");
                    server_model_.dirty("edit_error");
                    server_model_.dirty("show_add_form");
                    break;
                }
            }
        } else if (cmd == ID_DELETE) {
            settings_.delete_server(id);
            refresh_server_list();
        }
    };

    server_model_.on_generate_identity = [this]() {
        std::string phrase = parties::generate_seed_phrase();
        server_model_.seed_phrase = Rml::String(phrase);
        server_model_.show_onboarding = true;
        server_model_.show_restore = false;
        server_model_.dirty("seed_phrase");
        server_model_.dirty("show_onboarding");
        server_model_.dirty("show_restore");
    };

    server_model_.on_save_identity = [this]() {
        std::string phrase(server_model_.seed_phrase);
        SecretKey sk{};
        PublicKey pk{};
        if (!parties::derive_keypair(phrase, sk, pk)) {
            std::fprintf(stderr, "[App] Failed to derive keypair from seed phrase\n");
            return;
        }
        if (!settings_.save_identity(phrase, sk, pk)) {
            std::fprintf(stderr, "[App] Failed to save identity\n");
            return;
        }
        secret_key_ = sk;
        public_key_ = pk;
        has_identity_ = true;

        server_model_.fingerprint = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity = true;
        server_model_.show_onboarding = false;
        server_model_.dirty("fingerprint");
        server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding");

        std::printf("[App] Identity saved: %s\n",
                    parties::public_key_fingerprint(pk).c_str());
    };

    server_model_.on_restore_identity = [this]() {
        std::string phrase(server_model_.restore_phrase);
        if (!parties::validate_seed_phrase(phrase)) {
            server_model_.login_error = "Invalid seed phrase";
            server_model_.dirty("login_error");
            return;
        }
        SecretKey sk{};
        PublicKey pk{};
        if (!parties::derive_keypair(phrase, sk, pk)) {
            server_model_.login_error = "Failed to derive keypair";
            server_model_.dirty("login_error");
            return;
        }
        if (!settings_.save_identity(phrase, sk, pk)) {
            server_model_.login_error = "Failed to save identity";
            server_model_.dirty("login_error");
            return;
        }
        secret_key_ = sk;
        public_key_ = pk;
        has_identity_ = true;

        server_model_.fingerprint = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity = true;
        server_model_.show_onboarding = false;
        server_model_.show_restore = false;
        server_model_.login_error = "";
        server_model_.dirty("fingerprint");
        server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding");
        server_model_.dirty("show_restore");
        server_model_.dirty("login_error");

        std::printf("[App] Identity restored: %s\n",
                    parties::public_key_fingerprint(pk).c_str());
    };

    server_model_.on_show_restore = [this]() {
        server_model_.show_restore = true;
        server_model_.restore_phrase = "";
        server_model_.login_error = "";
        server_model_.dirty("show_restore");
        server_model_.dirty("restore_phrase");
        server_model_.dirty("login_error");
    };

    server_model_.on_copy_fingerprint = [this]() {
        std::string fp(server_model_.fingerprint);
        if (fp.empty()) return;
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, fp.size() + 1);
        if (hMem) {
            char* dst = static_cast<char*>(GlobalLock(hMem));
            std::memcpy(dst, fp.c_str(), fp.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    };
}

void App::refresh_server_list() {
    auto saved = settings_.get_saved_servers();
    server_model_.servers.clear();
    for (auto& s : saved) {
        ServerEntry entry;
        entry.id = s.id;
        entry.name = Rml::String(s.name);
        entry.host = Rml::String(s.host);
        entry.port = s.port;
        entry.last_username = Rml::String(s.last_username);
        // Compute initials (first 2 chars of name, uppercased)
        if (s.name.size() >= 2)
            entry.initials = Rml::String(s.name.substr(0, 2));
        else if (!s.name.empty())
            entry.initials = Rml::String(s.name.substr(0, 1));
        else
            entry.initials = "?";
        server_model_.servers.push_back(std::move(entry));
    }
    server_model_.dirty("servers");
}

bool App::init(HWND hwnd) {
    hwnd_ = hwnd;

    // Open client settings database
    settings_.open("parties_client.db");

    // Load identity if it exists
    if (settings_.has_identity()) {
        auto id = settings_.load_identity();
        if (id) {
            secret_key_ = id->secret_key;
            public_key_ = id->public_key;
            has_identity_ = true;
            std::printf("[App] Identity loaded: %s\n",
                        parties::public_key_fingerprint(public_key_).c_str());
        }
    }

    // Initialize UI
    if (!ui_.init(hwnd)) return false;

    // Set up data model callbacks and register with RmlUi context
    // (must happen before loading documents which reference data models)
    setup_model_callbacks();
    setup_server_model_callbacks();

    if (!server_model_.init(ui_.context())) {
        std::fprintf(stderr, "[App] Failed to create server list data model\n");
        return false;
    }
    if (!model_.init(ui_.context())) {
        std::fprintf(stderr, "[App] Failed to create lobby data model\n");
        return false;
    }

    // Initialize audio
    if (!audio_.init()) {
        std::fprintf(stderr, "[App] Audio init failed (non-fatal)\n");
    }

    // Populate device lists from audio engine
    auto capture_devs = audio_.get_capture_devices();
    auto playback_devs = audio_.get_playback_devices();
    for (auto& d : capture_devs)
        model_.capture_devices.push_back({Rml::String(d.name), d.index});
    for (auto& d : playback_devs)
        model_.playback_devices.push_back({Rml::String(d.name), d.index});

    // Load and apply saved audio preferences
    {
        auto pref = [&](const char* key) -> std::string {
            auto v = settings_.get_pref(key);
            return v.value_or("");
        };

        // Denoise
        std::string val = pref("audio.denoise");
        if (!val.empty()) {
            bool enabled = (val != "0");
            audio_.set_denoise_enabled(enabled);
            model_.denoise_enabled = enabled;
        }

        // Normalize
        val = pref("audio.normalize");
        if (!val.empty()) {
            bool enabled = (val != "0");
            audio_.set_normalize_enabled(enabled);
            model_.normalize_enabled = enabled;
        }
        val = pref("audio.normalize_target");
        if (!val.empty()) {
            float t = std::strtof(val.c_str(), nullptr);
            audio_.set_normalize_target(t);
            model_.normalize_target = t;
        }

        // VAD
        val = pref("audio.vad");
        if (!val.empty()) {
            bool enabled = (val != "0");
            audio_.set_vad_enabled(enabled);
            model_.vad_enabled = enabled;
        }
        val = pref("audio.vad_threshold");
        if (!val.empty()) {
            float t = std::strtof(val.c_str(), nullptr);
            audio_.set_vad_threshold(t);
            model_.vad_threshold = t;
        }

        // Push-to-Talk
        val = pref("audio.ptt");
        if (!val.empty())
            model_.ptt_enabled = (val != "0");
        val = pref("audio.ptt_key");
        if (!val.empty()) {
            model_.ptt_key = std::stoi(val);
            model_.ptt_key_name = Rml::String(vk_to_name(model_.ptt_key).c_str());
        }
        val = pref("audio.ptt_delay");
        if (!val.empty())
            model_.ptt_delay = static_cast<float>(std::stoi(val));

        // Find saved device by name
        val = pref("audio.capture_device");
        if (!val.empty()) {
            for (size_t i = 0; i < capture_devs.size(); i++) {
                if (capture_devs[i].name == val) {
                    audio_.set_capture_device(static_cast<int>(i));
                    model_.selected_capture = static_cast<int>(i);
                    break;
                }
            }
        }
        val = pref("audio.playback_device");
        if (!val.empty()) {
            for (size_t i = 0; i < playback_devs.size(); i++) {
                if (playback_devs[i].name == val) {
                    audio_.set_playback_device(static_cast<int>(i));
                    model_.selected_playback = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    // UI sound effects (own playback device, always running)
    sound_player_.init();

    // Wire audio to network (QUIC TLS encrypts in transit)
    audio_.set_mixer(&mixer_);
    audio_.on_encoded_frame = [this](const uint8_t* data, size_t len) {
        if (!authenticated_ || current_channel_ == 0) return;

        // Packet: [type(1)][opus_data(N)]
        std::vector<uint8_t> pkt(1 + len);
        pkt[0] = parties::protocol::VOICE_PACKET_TYPE;
        std::memcpy(pkt.data() + 1, data, len);
        net_.send_data(pkt.data(), pkt.size());
    };

    // Wire data receive -> voice mixer / video decoder
    net_.on_data_received = [this](const uint8_t* data, size_t len) {
        if (len < 1) return;
        uint8_t type = data[0];

        if (type == parties::protocol::VOICE_PACKET_TYPE) {
            // Format: [type(1)][sender_id(4)][opus_data(N)] — QUIC TLS encrypts in transit
            if (len < 6) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;

            mixer_.push_packet(sender_id, data + 5, len - 5);
        }
        else if (type == parties::protocol::VIDEO_FRAME_PACKET_TYPE) {
            // Format: [type(1)][sender_id(4)][frame_number(4)][timestamp(4)][flags(1)][w(2)][h(2)][codec(1)][data(N)]
            if (len < 19) return;  // 1 + 4 + 14 minimum
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            on_video_frame_received(sender_id, data + 5, len - 5);
        }
        else if (type == parties::protocol::VIDEO_CONTROL_TYPE) {
            // Format: [type(1)][subtype(1)][data...] — no sender_id prefix
            if (len >= 2 && data[1] == parties::protocol::VIDEO_CTL_PLI && encoder_) {
                encoder_->force_keyframe();
            }
        }
    };

    net_.on_resumption_ticket = [this](const uint8_t* ticket, size_t len) {
        settings_.save_resumption_ticket(server_host_, server_port_, ticket, len);
    };

    net_.on_disconnected = [this]() {
        // Clean up screen sharing
        sharing_screen_ = false;
        if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
        if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
        capture_targets_.clear();
        clear_all_sharers();
        video_frame_number_ = 0;

        authenticated_ = false;
        current_channel_ = 0;
        channel_key_ = {};
        audio_.stop();
        mixer_.clear();

        // Reset UI state
        model_.is_connected = false;
        model_.current_channel = 0;
        model_.current_channel_name.clear();
        model_.channels.clear();
        model_.is_muted = false;
        model_.is_deafened = false;
        model_.is_sharing = false;
        model_.someone_sharing = false;
        model_.sharers.clear();
        model_.viewing_sharer_id = 0;
        model_.show_settings = false;
        model_.show_share_picker = false;
        model_.show_create_channel = false;
        model_.my_role = 3;
        model_.can_manage_channels = false;
        model_.can_kick = false;
        model_.can_manage_roles = false;
        model_.admin_message.clear();
        model_.dirty_all();

        server_model_.connected_server_id = 0;
        server_model_.show_login = false;
        server_model_.show_add_form = false;
        server_model_.login_error = "";
        server_model_.login_status = "";
        server_model_.dirty("connected_server_id");
        server_model_.dirty("show_login");
        server_model_.dirty("show_add_form");
        server_model_.dirty("login_error");
        server_model_.dirty("login_status");
    };

    // Register custom video_frame element before loading documents
    video_instancer_ = std::make_unique<VideoElementInstancer>();
    Rml::Factory::RegisterElementInstancer("video_frame", video_instancer_.get());

    // Load UI document (single merged layout)
    doc_ = ui_.load_document("ui/lobby.rml");
    if (doc_) ui_.show_document(doc_);

    // Titlebar buttons are handled natively by Win32 WM_NCHITTEST
    // (HTMINBUTTON, HTMAXBUTTON, HTCLOSE) — no RmlUi event bindings needed.

    // Populate server list
    refresh_server_list();

    // Set initial identity state on model
    if (has_identity_) {
        server_model_.has_identity = true;
        server_model_.fingerprint = Rml::String(
            parties::public_key_fingerprint(public_key_));
        server_model_.dirty("has_identity");
        server_model_.dirty("fingerprint");
    }

    return true;
}

void App::shutdown() {
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    audio_.shutdown();
    net_.disconnect();
    ui_.shutdown();
    settings_.close();
}

void App::update() {
    // Process network messages
    if (net_.is_connected())
        process_server_messages();

    // PTT keybind capture (scan for any key press)
    if (model_.ptt_binding) {
        for (int vk = 1; vk < 256; vk++) {
            if (vk == VK_ESCAPE) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    model_.ptt_binding = false;
                    model_.dirty("ptt_binding");
                    break;
                }
                continue;
            }
            if (GetAsyncKeyState(vk) & 0x8000) {
                model_.ptt_key = vk;
                model_.ptt_key_name = Rml::String(vk_to_name(vk).c_str());
                model_.ptt_binding = false;
                model_.dirty("ptt_key");
                model_.dirty("ptt_key_name");
                model_.dirty("ptt_binding");
                settings_.set_pref("audio.ptt_key", std::to_string(vk));
                break;
            }
        }
    }

    // PTT polling — hold key to unmute, release to mute (with optional delay)
    if (model_.ptt_enabled && model_.ptt_key != 0 && current_channel_ != 0) {
        bool held = (GetAsyncKeyState(model_.ptt_key) & 0x8000) != 0;
        auto now = std::chrono::steady_clock::now();

        if (held) {
            ptt_held_ = true;
            if (audio_.is_muted()) {
                audio_.set_muted(false);
                model_.is_muted = false;
                model_.dirty("is_muted");
            }
        } else if (ptt_held_) {
            // Key just released — start delay timer
            ptt_held_ = false;
            ptt_release_time_ = now;
        }

        // Apply mute after delay expires
        if (!ptt_held_ && !audio_.is_muted()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - ptt_release_time_).count();
            if (elapsed >= static_cast<int64_t>(model_.ptt_delay)) {
                audio_.set_muted(true);
                model_.is_muted = true;
                model_.dirty("is_muted");
            }
        }
    }

    // QUIC datagrams are received via callbacks — no polling needed

    // Deliver latest decoded video frame via DComp video surface
    if (new_frame_available_ && doc_) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (new_frame_available_) {
            uint32_t w = shared_width_, h = shared_height_;
            new_frame_available_ = false;

            if (w > 0 && h > 0) {
                // Ensure DComp video surface exists
                if (!ui_.has_video_surface())
                    ui_.create_video_surface(w, h);

                // Convert I420 → BGRA for the DComp video swapchain
                bgra_buffer_.resize(w * h * 4);
                uint8_t* dst = bgra_buffer_.data();
                for (uint32_t row = 0; row < h; row++) {
                    const uint8_t* y_row = shared_y_.data() + row * shared_y_stride_;
                    const uint8_t* u_row = shared_u_.data() + (row / 2) * shared_uv_stride_;
                    const uint8_t* v_row = shared_v_.data() + (row / 2) * shared_uv_stride_;
                    for (uint32_t x = 0; x < w; x++) {
                        int Y = y_row[x];
                        int U = u_row[x / 2];
                        int V = v_row[x / 2];
                        int C = Y - 16;
                        int D = U - 128;
                        int E = V - 128;
                        int R = (298 * C + 459 * E + 128) >> 8;
                        int G = (298 * C -  55 * D - 136 * E + 128) >> 8;
                        int B = (298 * C + 541 * D + 128) >> 8;
                        *dst++ = static_cast<uint8_t>(std::clamp(B, 0, 255));
                        *dst++ = static_cast<uint8_t>(std::clamp(G, 0, 255));
                        *dst++ = static_cast<uint8_t>(std::clamp(R, 0, 255));
                        *dst++ = 255;
                    }
                }

                // Present to DComp video swapchain
                ui_.present_video_frame(bgra_buffer_.data(), w, h, w * 4);

                // Update VideoElement layout dimensions (no pixel data)
                auto* elem = doc_->GetElementById("screen-share");
                if (elem)
                    static_cast<VideoElement*>(elem)->SetVideoDimensions(w, h);
            }
        }
    }

    // Sync DComp video visual position with RmlUi element layout
    if (doc_ && ui_.has_video_surface()) {
        auto* elem = doc_->GetElementById("screen-share");
        if (elem) {
            auto* ve = static_cast<VideoElement*>(elem);
            if (ve->position_dirty()) {
                ui_.update_video_position(ve->render_x(), ve->render_y(),
                                          ve->render_w(), ve->render_h());
                ve->clear_position_dirty();
            }
        }
    }

    // Update voice level meter
    update_voice_level();

    // Update + render UI
    ui_.update();
    ui_.render();
}

void App::update_voice_level() {
    if (!doc_ || !model_.is_connected) return;

    float level = audio_.voice_level();
    model_.voice_level = level;
    model_.dirty("voice_level");

    // Update level meter bar width and threshold marker position directly
    if (auto* fill = doc_->GetElementById("voice-level-fill")) {
        int pct = static_cast<int>(level * 100.0f);
        if (pct > 100) pct = 100;
        fill->SetProperty("width", Rml::String(std::to_string(pct) + "%"));
    }
    if (auto* marker = doc_->GetElementById("vad-threshold-marker")) {
        int pct = static_cast<int>(model_.vad_threshold * 100.0f);
        marker->SetProperty("left", Rml::String(std::to_string(pct) + "%"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Connect action (from server list login overlay)
// ═══════════════════════════════════════════════════════════════════════

void App::do_connect() {
    if (!has_identity_) {
        server_model_.login_error = "No identity — generate seed phrase first";
        server_model_.dirty("login_error");
        return;
    }

    username_ = server_model_.login_username;

    server_model_.login_error = "";
    server_model_.dirty("login_error");

    // Connect TLS if not already connected
    if (!net_.is_connected()) {
        server_model_.login_status = "Connecting...";
        server_model_.dirty("login_status");

        // Load resumption ticket for 0-RTT if available
        auto ticket = settings_.load_resumption_ticket(server_host_, server_port_);
        if (!net_.connect(server_host_, server_port_,
                          ticket.empty() ? nullptr : ticket.data(), ticket.size())) {
            server_model_.login_error = "Failed to connect to server";
            server_model_.dirty("login_error");
            return;
        }

        // TOFU check
        std::string fp = net_.get_server_fingerprint();
        auto result = settings_.check_fingerprint(server_host_, server_port_, fp);
        if (result == Settings::TofuResult::Mismatch) {
            server_model_.login_error = "Server certificate changed! Possible MITM attack.";
            server_model_.dirty("login_error");
            settings_.delete_resumption_ticket(server_host_, server_port_);
            net_.disconnect();
            return;
        }
        if (result == Settings::TofuResult::Unknown) {
            settings_.trust_fingerprint(server_host_, server_port_, fp);
        }
    }

    server_model_.login_status = "Authenticating...";
    server_model_.dirty("login_status");

    send_auth_identity();
}

void App::send_auth_identity() {
    auto now = static_cast<uint64_t>(std::time(nullptr));

    // Build signed message: pubkey(32) + display_name + timestamp(8)
    BinaryWriter sig_msg;
    sig_msg.write_bytes(public_key_.data(), 32);
    sig_msg.write_string(username_);
    sig_msg.write_u64(now);

    Signature sig{};
    if (!parties::ed25519_sign(sig_msg.data().data(), sig_msg.data().size(),
                                secret_key_, public_key_, sig)) {
        server_model_.login_error = "Failed to sign auth message";
        server_model_.dirty("login_error");
        return;
    }

    // AUTH_IDENTITY: [pubkey(32)][display_name][timestamp(8)][signature(64)]
    BinaryWriter writer;
    writer.write_bytes(public_key_.data(), 32);
    writer.write_string(username_);
    writer.write_u64(now);
    writer.write_bytes(sig.data(), 64);

    net_.send_message(protocol::ControlMessageType::AUTH_IDENTITY,
                      writer.data().data(), writer.data().size());
}

// ═══════════════════════════════════════════════════════════════════════
// Channel operations
// ═══════════════════════════════════════════════════════════════════════

void App::join_channel(ChannelId id) {
    if (!authenticated_ || id == current_channel_) return;

    BinaryWriter writer;
    writer.write_u32(id);
    net_.send_message(protocol::ControlMessageType::CHANNEL_JOIN,
                      writer.data().data(), writer.data().size());
}

void App::leave_channel() {
    if (!authenticated_ || current_channel_ == 0) return;

    // Close share picker if open
    if (model_.show_share_picker) {
        model_.show_share_picker = false;
        model_.dirty("show_share_picker");
        capture_targets_.clear();
        if (capture_ && !sharing_screen_) { capture_->shutdown(); capture_.reset(); }
    }

    // Stop screen sharing if we're the sharer
    if (sharing_screen_)
        stop_screen_share();

    // Clean up all screen share viewer state
    clear_all_sharers();
    model_.is_sharing = false;
    model_.dirty("is_sharing");

    net_.send_message(protocol::ControlMessageType::CHANNEL_LEAVE, nullptr, 0);
    current_channel_ = 0;
    channel_key_ = {};
    sound_player_.play(SoundPlayer::Effect::LeaveChannel);
    audio_.stop();
    mixer_.clear();

    model_.current_channel = 0;
    model_.current_channel_name.clear();
    model_.dirty("current_channel");
    model_.dirty("current_channel_name");

    // Clear users from all channels in the model
    for (auto& ch : model_.channels) {
        ch.users.clear();
        ch.user_count = 0;
    }
    model_.dirty("channels");
}

// ═══════════════════════════════════════════════════════════════════════
// Server message processing
// ═══════════════════════════════════════════════════════════════════════

void App::process_server_messages() {
    auto messages = net_.incoming().drain();
    for (auto& msg : messages) {
        switch (msg.type) {
        case protocol::ControlMessageType::AUTH_RESPONSE:
            on_auth_response(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::CHANNEL_LIST:
            on_channel_list(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::CHANNEL_USER_LIST:
            on_channel_user_list(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::USER_JOINED_CHANNEL:
            on_user_joined(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::USER_LEFT_CHANNEL:
            on_user_left(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::USER_ROLE_CHANGED:
            on_user_role_changed(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::CHANNEL_KEY:
            on_channel_key(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::SCREEN_SHARE_STARTED:
            on_screen_share_started(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::SCREEN_SHARE_STOPPED:
            on_screen_share_stopped(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::SCREEN_SHARE_DENIED:
            on_screen_share_denied(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::SERVER_ERROR:
            on_server_error(msg.payload.data(), msg.payload.size());
            break;
        case protocol::ControlMessageType::ADMIN_RESULT:
            on_admin_result(msg.payload.data(), msg.payload.size());
            break;
        default:
            break;
        }
    }
}

void App::on_auth_response(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    user_id_ = reader.read_u32();

    uint8_t session_token[32];
    reader.read_bytes(session_token, 32);

    role_ = reader.read_u8();
    std::string server_name = reader.read_string();
    if (reader.error()) return;

    authenticated_ = true;

    settings_.save_server(server_name, server_host_, server_port_,
                          net_.get_server_fingerprint(), username_);
    refresh_server_list();

    // QUIC data plane is already connected (single connection)

    // Update model and switch to connected state
    model_.server_name = Rml::String(server_name);
    model_.username = Rml::String(username_);
    model_.is_connected = true;
    model_.my_role = role_;
    model_.can_manage_channels = (role_ <= static_cast<int>(parties::Role::Admin));
    model_.can_kick = (role_ <= static_cast<int>(parties::Role::Admin));
    model_.can_manage_roles = (role_ <= static_cast<int>(parties::Role::Admin));
    model_.dirty("server_name");
    model_.dirty("username");
    model_.dirty("is_connected");
    model_.dirty("my_role");
    model_.dirty("can_manage_channels");
    model_.dirty("can_kick");
    model_.dirty("can_manage_roles");

    server_model_.connected_server_id = connecting_server_id_;
    server_model_.show_login = false;
    server_model_.login_error = "";
    server_model_.login_status = "";
    server_model_.dirty("connected_server_id");
    server_model_.dirty("show_login");
    server_model_.dirty("login_error");
    server_model_.dirty("login_status");
}


void App::on_channel_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    model_.channels.clear();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ch_id = reader.read_u32();
        std::string name = reader.read_string();
        uint32_t max_users = reader.read_u32();
        uint32_t sort_order = reader.read_u32();
        uint32_t user_count = reader.read_u32();
        if (reader.error()) break;

        (void)sort_order;

        ChannelInfo ch;
        ch.id = static_cast<int>(ch_id);
        ch.name = Rml::String(name);
        ch.max_users = static_cast<int>(max_users);
        ch.user_count = static_cast<int>(user_count);
        model_.channels.push_back(std::move(ch));
    }

    model_.dirty("channels");
}

void App::on_channel_user_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    ChannelId channel_id = reader.read_u32();
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    current_channel_ = channel_id;
    model_.current_channel = static_cast<int>(channel_id);

    // Clear users from all channels (we only get user lists for our channel)
    for (auto& ch : model_.channels) {
        ch.users.clear();
        ch.user_count = 0;
    }

    // Find the channel and populate its users
    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            model_.current_channel_name = ch.name;

            ch.users.clear();
            for (uint32_t i = 0; i < count; i++) {
                uint32_t uid = reader.read_u32();
                std::string uname = reader.read_string();
                uint8_t urole = reader.read_u8();
                uint8_t muted = reader.read_u8();
                uint8_t deafened = reader.read_u8();
                if (reader.error()) break;

                ChannelUser user;
                user.id = static_cast<int>(uid);
                user.name = Rml::String(uname);
                user.role = urole;
                user.muted = muted != 0;
                user.deafened = deafened != 0;
                ch.users.push_back(std::move(user));
            }
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }

    model_.dirty("current_channel");
    model_.dirty("current_channel_name");
    model_.dirty("channels");

    audio_.start();
    sound_player_.play(SoundPlayer::Effect::JoinChannel);
}

void App::on_user_joined(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t uid = reader.read_u32();
    std::string uname = reader.read_string();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ChannelUser user;
            user.id = static_cast<int>(uid);
            user.name = Rml::String(uname);
            ch.users.push_back(std::move(user));
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }

    if (channel_id == current_channel_)
        sound_player_.play(SoundPlayer::Effect::UserJoined);

    model_.dirty("channels");
}

void App::on_user_left(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t uid = reader.read_u32();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    mixer_.remove_user(uid);

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            auto& users = ch.users;
            for (auto it = users.begin(); it != users.end(); ++it) {
                if (it->id == static_cast<int>(uid)) {
                    users.erase(it);
                    break;
                }
            }
            ch.user_count = static_cast<int>(users.size());
            break;
        }
    }

    if (channel_id == current_channel_)
        sound_player_.play(SoundPlayer::Effect::UserLeft);

    model_.dirty("channels");
}

void App::on_user_role_changed(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t uid = reader.read_u32();
    uint8_t new_role = reader.read_u8();
    if (reader.error()) return;

    // Update own role if it's us
    if (uid == user_id_) {
        role_ = new_role;
        model_.my_role = new_role;
        model_.can_manage_channels = (new_role <= static_cast<int>(parties::Role::Admin));
        model_.can_kick = (new_role <= static_cast<int>(parties::Role::Admin));
        model_.can_manage_roles = (new_role <= static_cast<int>(parties::Role::Admin));
        model_.dirty("my_role");
        model_.dirty("can_manage_channels");
        model_.dirty("can_kick");
        model_.dirty("can_manage_roles");
    }

    // Update in channel user list
    for (auto& ch : model_.channels) {
        for (auto& user : ch.users) {
            if (user.id == static_cast<int>(uid)) {
                user.role = new_role;
            }
        }
    }
    model_.dirty("channels");
}

void App::on_channel_key(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    ChannelId ch_id = reader.read_u32();
    if (reader.remaining() < 32 || reader.error()) return;
    reader.read_bytes(channel_key_.data(), 32);
}

void App::on_server_error(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    std::string message = reader.read_string();
    if (reader.error()) return;

    std::fprintf(stderr, "[App] Server error: %s\n", message.c_str());

    if (server_model_.show_login) {
        server_model_.login_error = Rml::String(message);
        server_model_.login_status = "";
        server_model_.dirty("login_error");
        server_model_.dirty("login_status");
    }
}

void App::on_admin_result(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint8_t success = reader.read_u8();
    std::string message = reader.read_string();
    if (reader.error()) return;

    std::printf("[App] Admin result: %s — %s\n",
                success ? "OK" : "FAIL", message.c_str());

    model_.admin_message = Rml::String(message);
    model_.dirty("admin_message");
}

// ═══════════════════════════════════════════════════════════════════════
// Screen sharing
// ═══════════════════════════════════════════════════════════════════════

void App::show_share_picker() {
    if (sharing_screen_ || !authenticated_ || current_channel_ == 0) return;

    // Enumerate available capture targets
    capture_ = std::make_unique<ScreenCapture>();
    if (!capture_->init()) {
        std::fprintf(stderr, "[App] Screen capture init failed\n");
        capture_.reset();
        return;
    }

    capture_targets_.clear();
    model_.share_targets.clear();

    auto monitors = capture_->enumerate_monitors();
    for (auto& m : monitors) {
        int idx = static_cast<int>(capture_targets_.size());
        ShareTarget st;
        st.name = Rml::String(m.name);
        st.index = idx;
        st.is_monitor = true;
        model_.share_targets.push_back(std::move(st));
        capture_targets_.push_back(std::move(m));
    }

    auto windows = capture_->enumerate_windows();
    for (auto& w : windows) {
        int idx = static_cast<int>(capture_targets_.size());
        ShareTarget st;
        st.name = Rml::String(w.name);
        st.index = idx;
        st.is_monitor = false;
        model_.share_targets.push_back(std::move(st));
        capture_targets_.push_back(std::move(w));
    }

    model_.show_share_picker = true;
    model_.dirty("share_targets");
    model_.dirty("show_share_picker");
}

void App::start_screen_share(int target_index) {
    if (sharing_screen_ || !authenticated_ || current_channel_ == 0) return;

    if (target_index < 0 || target_index >= static_cast<int>(capture_targets_.size())) {
        capture_targets_.clear();
        if (capture_) { capture_->shutdown(); capture_.reset(); }
        return;
    }

    // capture_ was already initialized in show_share_picker()
    if (!capture_) return;

    if (!capture_->start(capture_targets_[target_index])) {
        std::fprintf(stderr, "[App] Failed to start capture\n");
        capture_->shutdown();
        capture_.reset();
        capture_targets_.clear();
        return;
    }

    capture_targets_.clear();

    uint32_t width = capture_->width();
    uint32_t height = capture_->height();

    // Create encoder (uses same D3D device as capture)
    encoder_ = std::make_unique<VideoEncoder>();
    if (!encoder_->init(capture_->device(), width, height)) {
        std::fprintf(stderr, "[App] Video encoder init failed\n");
        capture_->stop();
        capture_->shutdown();
        capture_.reset();
        encoder_.reset();
        return;
    }

    video_frame_number_ = 0;

    // Wire capture -> encoder (only encodes when sharing_screen_ is true)
    capture_->on_frame = [this](ID3D11Texture2D* texture, uint32_t w, uint32_t h) {
        if (!encoder_ || !sharing_screen_) return;
        int64_t ts = static_cast<int64_t>(video_frame_number_) * 333333; // ~30fps in 100ns units
        encoder_->encode_frame(texture, ts);
    };

    // Wire encoder -> network send (QUIC TLS encrypts in transit)
    encoder_->on_encoded = [this](const uint8_t* data, size_t len, bool keyframe) {
        if (!sharing_screen_ || !authenticated_) return;

        // Packet: [type(1)][frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec_id(1)][data(N)]
        uint32_t fn = video_frame_number_++;
        uint32_t ts = fn;
        uint8_t flags = keyframe ? VIDEO_FLAG_KEYFRAME : 0;
        uint16_t w = static_cast<uint16_t>(encoder_->width());
        uint16_t h = static_cast<uint16_t>(encoder_->height());
        uint8_t codec = static_cast<uint8_t>(encoder_->codec());

        size_t header_len = 1 + 4 + 4 + 1 + 2 + 2 + 1;
        std::vector<uint8_t> pkt(header_len + len);
        size_t off = 0;
        pkt[off++] = protocol::VIDEO_FRAME_PACKET_TYPE;
        std::memcpy(pkt.data() + off, &fn, 4); off += 4;
        std::memcpy(pkt.data() + off, &ts, 4); off += 4;
        pkt[off++] = flags;
        std::memcpy(pkt.data() + off, &w, 2); off += 2;
        std::memcpy(pkt.data() + off, &h, 2); off += 2;
        pkt[off++] = codec;
        std::memcpy(pkt.data() + off, data, len);

        net_.send_video(pkt.data(), pkt.size(), true);

        // Feed raw bitstream to local preview decoder (no encryption needed)
        if (decoder_ && viewing_sharer_ == user_id_) {
            std::lock_guard<std::mutex> lock(decode_queue_mutex_);
            decode_queue_.push({std::vector<uint8_t>(data, data + len), 0});
            decode_queue_cv_.notify_one();
        }
    };

    // Send SCREEN_SHARE_START to server (don't set sharing_screen_ until confirmed)
    VideoCodecId codec = encoder_->codec();
    BinaryWriter writer;
    writer.write_u8(static_cast<uint8_t>(codec));
    writer.write_u16(static_cast<uint16_t>(width));
    writer.write_u16(static_cast<uint16_t>(height));
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_START,
                      writer.data().data(), writer.data().size());
}

void App::stop_screen_share() {
    if (!sharing_screen_) return;

    sharing_screen_ = false;

    // Stop self-preview
    if (viewing_sharer_ == user_id_) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        ui_.destroy_video_surface();
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
        viewing_sharer_ = 0;
    }

    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    video_frame_number_ = 0;

    // Remove self from active sharers
    active_sharers_.erase(user_id_);

    model_.is_sharing = false;
    model_.dirty("is_sharing");
    sync_sharer_model();

    // Notify server
    if (authenticated_ && current_channel_ != 0)
        net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

void App::on_video_frame_received(uint32_t sender_id, const uint8_t* data, size_t len) {
    // data = [frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec_id(1)][encoded(N)]
    if (len < 14) return;
    if (sender_id != viewing_sharer_) return;

    uint32_t frame_number;
    std::memcpy(&frame_number, data, 4);
    int64_t timestamp = static_cast<int64_t>(frame_number);

    const uint8_t* encoded = data + 14;
    size_t encoded_len = len - 14;

    if (decode_running_ && encoded_len > 0) {
        DecodeWork work;
        work.data.assign(encoded, encoded + encoded_len);
        work.timestamp = timestamp;

        {
            std::lock_guard<std::mutex> lock(decode_queue_mutex_);
            decode_queue_.push(std::move(work));
        }
        decode_queue_cv_.notify_one();
    }
}

void App::on_screen_share_started(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t sharer_id = reader.read_u32();
    uint8_t codec_id = reader.read_u8();
    uint16_t width = reader.read_u16();
    uint16_t height = reader.read_u16();
    if (reader.error()) return;

    if (sharer_id == user_id_) {
        // Server approved our share request
        sharing_screen_ = true;
        model_.is_sharing = true;
        model_.dirty("is_sharing");

        // Add self to active sharers (appears as a tab)
        SharerInfo self_info;
        self_info.user_id = user_id_;
        self_info.name = username_;
        self_info.codec = encoder_->codec();
        self_info.width = static_cast<uint16_t>(encoder_->width());
        self_info.height = static_cast<uint16_t>(encoder_->height());
        active_sharers_[user_id_] = std::move(self_info);

        // Start local preview: decode our own encoded stream
        viewing_sharer_ = user_id_;
        decoder_ = std::make_unique<VideoDecoder>();
        if (decoder_->init(encoder_->codec(), encoder_->width(), encoder_->height())) {
            start_decode_thread();
        }
        sync_sharer_model();
        return;
    }

    // Someone else started sharing — add to active sharers
    SharerInfo info;
    info.user_id = sharer_id;
    info.codec = static_cast<VideoCodecId>(codec_id);
    info.width = width;
    info.height = height;

    // Look up sharer username from channel user list
    info.name = "Unknown";
    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(current_channel_)) {
            for (auto& u : ch.users) {
                if (u.id == static_cast<int>(sharer_id)) {
                    info.name = u.name.c_str();
                    break;
                }
            }
            break;
        }
    }

    active_sharers_[sharer_id] = std::move(info);
    sync_sharer_model();
}

void App::on_screen_share_stopped(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t sharer_id = reader.read_u32();
    if (reader.error()) return;

    if (sharer_id == user_id_) {
        // Our share was stopped externally (e.g. we were kicked from channel)
        sharing_screen_ = false;
        if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
        if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
        video_frame_number_ = 0;
        model_.is_sharing = false;
        model_.dirty("is_sharing");
    }

    // Remove from active sharers
    active_sharers_.erase(sharer_id);

    // If we were watching this sharer, stop decoding
    if (sharer_id == viewing_sharer_) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
        viewing_sharer_ = 0;
    }

    sync_sharer_model();
}

void App::on_screen_share_denied(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    std::string reason = reader.read_string();
    if (reader.error()) return;

    std::fprintf(stderr, "[App] Screen share denied: %s\n", reason.c_str());

    // Clean up capture/encoder we set up optimistically
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    capture_targets_.clear();
    video_frame_number_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-sharer subscribe / stream switching
// ═══════════════════════════════════════════════════════════════════════

void App::watch_sharer(UserId id) {
    auto it = active_sharers_.find(id);
    if (it == active_sharers_.end() || id == viewing_sharer_) return;

    // Stop current decode if switching
    if (viewing_sharer_ != 0) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        ui_.destroy_video_surface();
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
    }

    viewing_sharer_ = id;

    auto& info = it->second;
    decoder_ = std::make_unique<VideoDecoder>();
    if (!decoder_->init(info.codec, info.width, info.height)) {
        std::fprintf(stderr, "[App] Video decoder init failed (codec=%u, %ux%u)\n",
                     static_cast<unsigned>(info.codec), info.width, info.height);
        decoder_.reset();
        viewing_sharer_ = 0;
        sync_sharer_model();
        return;
    }
    start_decode_thread();

    if (id == user_id_ && sharing_screen_) {
        // Self-preview: encoder callback feeds decode queue directly, no network subscribe needed
    } else {
        // Remote sharer: subscribe via server and request keyframe
        BinaryWriter writer;
        writer.write_u32(id);
        net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                          writer.data().data(), writer.data().size());
        send_pli(id);
    }

    sync_sharer_model();
}

void App::stop_watching() {
    if (viewing_sharer_ == 0) return;

    bool was_self = (viewing_sharer_ == user_id_);

    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    ui_.destroy_video_surface();
    if (doc_) {
        auto* elem = doc_->GetElementById("screen-share");
        if (elem) static_cast<VideoElement*>(elem)->Clear();
    }

    viewing_sharer_ = 0;

    // Send unsubscribe to server (only for remote sharers)
    if (!was_self) {
        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(0));
        net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                          writer.data().data(), writer.data().size());
    }

    sync_sharer_model();
}

void App::send_pli(UserId target) {
    // [VIDEO_CONTROL_TYPE(1)][VIDEO_CTL_PLI(1)][target_user_id(4)]
    std::vector<uint8_t> pkt(6);
    pkt[0] = protocol::VIDEO_CONTROL_TYPE;
    pkt[1] = protocol::VIDEO_CTL_PLI;
    std::memcpy(pkt.data() + 2, &target, 4);
    net_.send_video(pkt.data(), pkt.size(), true);
}

void App::clear_all_sharers() {
    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    ui_.destroy_video_surface();
    active_sharers_.clear();
    viewing_sharer_ = 0;
    if (doc_) {
        auto* elem = doc_->GetElementById("screen-share");
        if (elem) static_cast<VideoElement*>(elem)->Clear();
    }
    model_.sharers.clear();
    model_.someone_sharing = false;
    model_.viewing_sharer_id = 0;
    model_.dirty("sharers");
    model_.dirty("someone_sharing");
    model_.dirty("viewing_sharer_id");
}

void App::sync_sharer_model() {
    model_.sharers.clear();
    for (auto& [uid, info] : active_sharers_) {
        ActiveSharer as;
        as.id = static_cast<int>(uid);
        as.name = Rml::String(info.name.c_str());
        model_.sharers.push_back(std::move(as));
    }
    model_.someone_sharing = !active_sharers_.empty();
    model_.viewing_sharer_id = static_cast<int>(viewing_sharer_);
    model_.dirty("sharers");
    model_.dirty("someone_sharing");
    model_.dirty("viewing_sharer_id");
}

// ═══════════════════════════════════════════════════════════════════════
// Video decode thread
// ═══════════════════════════════════════════════════════════════════════

void App::start_decode_thread() {
    if (decode_running_) return;

    // Set up decoder callback to copy frames into shared buffer
    decoder_->on_decoded = [this](const DecodedFrame& frame) {
        uint32_t half_h = frame.height / 2;

        std::lock_guard<std::mutex> lock(frame_mutex_);
        shared_y_.resize(frame.y_stride * frame.height);
        std::memcpy(shared_y_.data(), frame.y_plane, frame.y_stride * frame.height);

        shared_u_.resize(frame.uv_stride * half_h);
        std::memcpy(shared_u_.data(), frame.u_plane, frame.uv_stride * half_h);

        shared_v_.resize(frame.uv_stride * half_h);
        std::memcpy(shared_v_.data(), frame.v_plane, frame.uv_stride * half_h);

        shared_width_ = frame.width;
        shared_height_ = frame.height;
        shared_y_stride_ = frame.y_stride;
        shared_uv_stride_ = frame.uv_stride;
        new_frame_available_ = true;
    };

    decode_running_ = true;
    decode_thread_ = std::thread([this]() { decode_loop(); });
}

void App::stop_decode_thread() {
    if (!decode_running_) return;

    decode_running_ = false;
    decode_queue_cv_.notify_all();

    if (decode_thread_.joinable())
        decode_thread_.join();

    // Drain the queue
    {
        std::lock_guard<std::mutex> lock(decode_queue_mutex_);
        while (!decode_queue_.empty()) decode_queue_.pop();
    }
}

void App::decode_loop() {
    while (decode_running_) {
        // Grab ALL queued frames at once
        std::queue<DecodeWork> batch;
        {
            std::unique_lock<std::mutex> lock(decode_queue_mutex_);
            decode_queue_cv_.wait(lock, [this]() {
                return !decode_queue_.empty() || !decode_running_;
            });
            if (!decode_running_) break;
            batch.swap(decode_queue_);
        }

        // Decode every frame (AV1 needs all frames for inter-prediction)
        // The on_decoded callback overwrites the shared buffer each time,
        // so the main thread only sees the latest decoded result.
        while (!batch.empty()) {
            auto& work = batch.front();
            if (decoder_ && !work.data.empty())
                decoder_->decode(work.data.data(), work.data.size(), work.timestamp);
            batch.pop();
            if (!decode_running_) break;
        }
    }
}

} // namespace parties::client
