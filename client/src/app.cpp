#include <client/app.h>
#include <client/context_menu.h>
#include <client/screen_capture.h>
#include <client/video_encoder.h>
#include <client/video_decoder.h>
#include <client/video_element.h>
#include <client/level_meter_element.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/profiler.h>

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
        // Update own entry in channel user list
        for (auto& ch : model_.channels) {
            for (auto& u : ch.users) {
                if (u.id == static_cast<int>(user_id_)) u.muted = muted;
            }
        }
        model_.dirty("channels");
        sound_player_.play(muted ? SoundPlayer::Effect::Mute
                                 : SoundPlayer::Effect::Unmute);
        send_voice_state();
    };

    model_.on_toggle_deafen = [this]() {
        bool deafened = !audio_.is_deafened();
        audio_.set_deafened(deafened);
        model_.is_deafened = deafened;
        model_.dirty("is_deafened");
        // Update own entry in channel user list
        for (auto& ch : model_.channels) {
            for (auto& u : ch.users) {
                if (u.id == static_cast<int>(user_id_)) u.deafened = deafened;
            }
        }
        model_.dirty("channels");
        sound_player_.play(deafened ? SoundPlayer::Effect::Deafen
                                    : SoundPlayer::Effect::Undeafen);
        send_voice_state();
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
        save_pref_debounced("audio.normalize_target", std::to_string(target));
    };

    model_.on_aec_changed = [this](bool enabled) {
        audio_.set_aec_enabled(enabled);
        settings_.set_pref("audio.aec", enabled ? "1" : "0");
    };

    model_.on_vad_changed = [this](bool enabled) {
        audio_.set_vad_enabled(enabled);
        settings_.set_pref("audio.vad", enabled ? "1" : "0");
    };

    model_.on_vad_threshold_changed = [this](float threshold) {
        audio_.set_vad_threshold(threshold);
        save_pref_debounced("audio.vad_threshold", std::to_string(threshold));
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
        save_pref_debounced("audio.ptt_delay", std::to_string(static_cast<int>(delay)));
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

    model_.on_stream_volume_changed = [this](float vol) {
        stream_audio_player_.set_volume(vol);
        save_pref_debounced("audio.stream_volume", std::to_string(vol));
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
        // Don't show menu for ourselves
        if (static_cast<uint32_t>(user_id) == user_id_) return;

        bool can_roles = model_.can_manage_roles && role_ <= user_role;
        bool can_kick_user = model_.can_kick && role_ <= user_role;

        model_.menu_user_id = user_id;
        model_.menu_user_name = name;
        model_.menu_user_role = user_role;
        model_.menu_can_roles = can_roles;
        model_.menu_can_kick = can_kick_user;
        // Apply saved per-user audio prefs to mixer (in case stream was
        // created after join and hasn't had prefs applied yet)
        apply_user_audio_prefs(static_cast<UserId>(user_id));

        // Retrieve current volume and compression state from mixer
        model_.menu_user_volume = mixer_.get_user_volume(static_cast<UserId>(user_id));
        model_.menu_user_compress = mixer_.get_user_compression(static_cast<UserId>(user_id));
        model_.menu_user_compress_target = mixer_.get_user_compression_target(static_cast<UserId>(user_id));
        model_.show_user_menu = true;

        model_.dirty("menu_user_id");
        model_.dirty("menu_user_name");
        model_.dirty("menu_user_role");
        model_.dirty("menu_can_roles");
        model_.dirty("menu_can_kick");
        model_.dirty("menu_user_volume");
        model_.dirty("menu_user_compress");
        model_.dirty("menu_user_compress_target");
        model_.dirty("show_user_menu");
    };

    model_.on_set_user_role = [this](int user_id, int new_role) {
        if (!authenticated_) return;
        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(user_id));
        writer.write_u8(static_cast<uint8_t>(new_role));
        net_.send_message(protocol::ControlMessageType::ADMIN_SET_ROLE,
                          writer.data().data(), writer.data().size());
    };

    model_.on_kick_user = [this](int user_id) {
        if (!authenticated_) return;
        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(user_id));
        net_.send_message(protocol::ControlMessageType::ADMIN_KICK_USER,
                          writer.data().data(), writer.data().size());
    };

    model_.on_user_volume_changed = [this](int user_id, float volume) {
        mixer_.set_user_volume(static_cast<UserId>(user_id), volume);
        auto key = "user." + std::to_string(user_id) + ".volume";
        save_pref_debounced(key, std::to_string(volume));
    };

    model_.on_user_compress_changed = [this](int user_id, bool enabled, float target) {
        mixer_.set_user_compression(static_cast<UserId>(user_id), enabled, target);
        auto prefix = "user." + std::to_string(user_id);
        save_pref_debounced(prefix + ".compress", enabled ? "1" : "0");
        save_pref_debounced(prefix + ".compress_target", std::to_string(target));
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

    // Identity backup/import
    model_.on_show_seed_phrase = [this]() {
        if (!has_identity_) return;
        if (model_.show_seed_phrase) {
            // Toggle off
            model_.show_seed_phrase = false;
            model_.identity_seed_phrase = "";
            model_.dirty("show_seed_phrase");
            model_.dirty("identity_seed_phrase");
            return;
        }
        auto id = settings_.load_identity();
        if (!id) return;
        model_.identity_seed_phrase = Rml::String(id->seed_phrase);
        model_.show_seed_phrase = true;
        model_.dirty("identity_seed_phrase");
        model_.dirty("show_seed_phrase");
    };

    model_.on_copy_seed_phrase = [this]() {
        std::string phrase(model_.identity_seed_phrase);
        if (phrase.empty()) return;
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, phrase.size() + 1);
        if (hMem) {
            char* dst = static_cast<char*>(GlobalLock(hMem));
            std::memcpy(dst, phrase.c_str(), phrase.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    };

    model_.on_show_private_key = [this]() {
        if (!has_identity_) return;
        if (model_.show_private_key) {
            model_.show_private_key = false;
            model_.identity_private_key = "";
            model_.dirty("show_private_key");
            model_.dirty("identity_private_key");
            return;
        }
        model_.identity_private_key = Rml::String(parties::secret_key_to_hex(secret_key_));
        model_.show_private_key = true;
        model_.dirty("identity_private_key");
        model_.dirty("show_private_key");
    };

    model_.on_copy_private_key = [this]() {
        std::string key_hex(model_.identity_private_key);
        if (key_hex.empty()) return;
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, key_hex.size() + 1);
        if (hMem) {
            char* dst = static_cast<char*>(GlobalLock(hMem));
            std::memcpy(dst, key_hex.c_str(), key_hex.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    };

    model_.on_show_import = [this]() {
        model_.show_import_identity = true;
        model_.import_phrase = "";
        model_.import_error = "";
        model_.dirty("show_import_identity");
        model_.dirty("import_phrase");
        model_.dirty("import_error");
    };

    model_.on_do_import = [this]() {
        std::string input(model_.import_phrase);
        SecretKey sk{};
        PublicKey pk{};
        std::string seed_phrase;

        // Try as 64-char hex private key first, then as seed phrase
        if (input.size() == 64 && parties::secret_key_from_hex(input, sk)) {
            if (!parties::derive_pubkey(sk, pk)) {
                model_.import_error = "Failed to derive public key";
                model_.dirty("import_error");
                return;
            }
            seed_phrase = "";  // no seed phrase for raw key import
        } else if (parties::validate_seed_phrase(input)) {
            if (!parties::derive_keypair(input, sk, pk)) {
                model_.import_error = "Failed to derive keypair";
                model_.dirty("import_error");
                return;
            }
            seed_phrase = input;
        } else {
            model_.import_error = "Enter a 12-word seed phrase or 64-char hex private key.";
            model_.dirty("import_error");
            return;
        }

        if (!settings_.save_identity(seed_phrase, sk, pk)) {
            model_.import_error = "Failed to save identity";
            model_.dirty("import_error");
            return;
        }
        secret_key_ = sk;
        public_key_ = pk;
        has_identity_ = true;

        // Update server list model fingerprint
        server_model_.fingerprint = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity = true;
        server_model_.dirty("fingerprint");
        server_model_.dirty("has_identity");

        // Reset import form and seed phrase display
        model_.show_import_identity = false;
        model_.import_phrase = "";
        model_.import_error = "";
        model_.show_seed_phrase = false;
        model_.identity_seed_phrase = "";
        model_.show_private_key = false;
        model_.identity_private_key = "";
        model_.dirty("show_import_identity");
        model_.dirty("import_phrase");
        model_.dirty("import_error");
        model_.dirty("show_seed_phrase");
        model_.dirty("identity_seed_phrase");
        model_.dirty("show_private_key");
        model_.dirty("identity_private_key");

        std::printf("[App] Identity imported: %s\n",
                    parties::public_key_fingerprint(pk).c_str());
    };

    model_.on_cancel_import = [this]() {
        model_.show_import_identity = false;
        model_.import_phrase = "";
        model_.import_error = "";
        model_.dirty("show_import_identity");
        model_.dirty("import_phrase");
        model_.dirty("import_error");
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
        auto& host = server_model_.edit_host;
        auto& port_str = server_model_.edit_port;

        if (host.empty() || port_str.empty()) {
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

        // Use host:port as placeholder name (real name comes from server on connect)
        std::string save_name = std::string(host) + ":" + std::string(port_str);
        settings_.save_server(save_name, std::string(host), port, "", "");

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
        constexpr int ID_DELETE = 1;
        std::vector<ContextMenu::Item> items;
        items.push_back({L"Delete", ID_DELETE, true});

        int cmd = ContextMenu::show(hwnd_, items);

        if (cmd == ID_DELETE) {
            settings_.delete_server(id);
            refresh_server_list();
        }
    };

    server_model_.on_generate_identity = [this]() {
        std::string phrase = parties::generate_seed_phrase();
        server_model_.seed_phrase = Rml::String(phrase);
        server_model_.show_onboarding = true;
        server_model_.show_restore = false;
        server_model_.show_key_import = false;
        server_model_.dirty("seed_phrase");
        server_model_.dirty("show_onboarding");
        server_model_.dirty("show_restore");
        server_model_.dirty("show_key_import");
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

    server_model_.on_show_key_import = [this]() {
        server_model_.show_key_import = true;
        server_model_.show_restore = false;
        server_model_.import_key_hex = "";
        server_model_.login_error = "";
        server_model_.dirty("show_key_import");
        server_model_.dirty("show_restore");
        server_model_.dirty("import_key_hex");
        server_model_.dirty("login_error");
    };

    server_model_.on_import_key = [this]() {
        std::string hex(server_model_.import_key_hex);
        SecretKey sk{};
        PublicKey pk{};
        if (!parties::secret_key_from_hex(hex, sk)) {
            server_model_.login_error = "Invalid private key. Must be 64 hex characters.";
            server_model_.dirty("login_error");
            return;
        }
        if (!parties::derive_pubkey(sk, pk)) {
            server_model_.login_error = "Failed to derive public key";
            server_model_.dirty("login_error");
            return;
        }
        if (!settings_.save_identity("", sk, pk)) {
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
        server_model_.show_key_import = false;
        server_model_.login_error = "";
        server_model_.dirty("fingerprint");
        server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding");
        server_model_.dirty("show_key_import");
        server_model_.dirty("login_error");

        std::printf("[App] Identity imported from key: %s\n",
                    parties::public_key_fingerprint(pk).c_str());
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

    server_model_.on_tofu_accept = [this]() {
        // User chose to trust the new certificate
        std::string fp(server_model_.tofu_fingerprint);
        settings_.trust_fingerprint(server_host_, server_port_, fp);
        settings_.delete_resumption_ticket(server_host_, server_port_);

        server_model_.show_tofu_warning = false;
        server_model_.show_login = true;
        server_model_.login_status = "Authenticating...";
        server_model_.dirty("show_tofu_warning");
        server_model_.dirty("show_login");
        server_model_.dirty("login_status");

        send_auth_identity();
    };

    server_model_.on_tofu_reject = [this]() {
        // User rejected the new certificate
        server_model_.show_tofu_warning = false;
        server_model_.dirty("show_tofu_warning");
        net_.disconnect();
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

    // Highlight the system default devices initially
    model_.selected_capture = audio_.default_capture_index();
    model_.selected_playback = audio_.default_playback_index();

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

        // AEC
        val = pref("audio.aec");
        if (!val.empty()) {
            bool enabled = (val != "0");
            audio_.set_aec_enabled(enabled);
            model_.aec_enabled = enabled;
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

    // Stream audio decoder (mixed into AudioEngine's playback device)
    stream_audio_player_.init();

    // Load saved stream volume
    {
        auto vol_str = settings_.get_pref("audio.stream_volume");
        if (vol_str) {
            float vol = std::strtof(vol_str->c_str(), nullptr);
            stream_audio_player_.set_volume(vol);
            model_.stream_volume = vol;
        }
    }

    // Load saved share settings (bitrate, fps)
    {
        auto bitrate_str = settings_.get_pref("video.share_bitrate");
        if (bitrate_str) model_.share_bitrate = std::strtof(bitrate_str->c_str(), nullptr);
        auto fps_str = settings_.get_pref("video.share_fps");
        if (fps_str) model_.share_fps = std::atoi(fps_str->c_str());
    }

    // Wire audio to network (QUIC TLS encrypts in transit)
    audio_.set_mixer(&mixer_);
    audio_.set_stream_player(&stream_audio_player_);
    audio_.on_encoded_frame = [this](const uint8_t* data, size_t len) {
        if (!authenticated_ || current_channel_ == 0) return;

        // Packet: [type(1)][seq(2)][opus_data(N)]
        uint16_t seq = voice_seq_++;
        std::vector<uint8_t> pkt(1 + 2 + len);
        pkt[0] = parties::protocol::VOICE_PACKET_TYPE;
        std::memcpy(pkt.data() + 1, &seq, 2);
        std::memcpy(pkt.data() + 3, data, len);
        net_.send_data(pkt.data(), pkt.size());
    };

    // Wire data receive -> voice mixer / video decoder
    net_.on_data_received = [this](const uint8_t* data, size_t len) {
        if (len < 1) return;
        uint8_t type = data[0];

        if (type == parties::protocol::VOICE_PACKET_TYPE) {
            // Format: [type(1)][sender_id(4)][seq(2)][opus_data(N)]
            if (len < 8) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;

            uint16_t seq;
            std::memcpy(&seq, data + 5, 2);
            mixer_.push_packet(sender_id, seq, data + 7, len - 7);
        }
        else if (type == parties::protocol::VIDEO_FRAME_PACKET_TYPE) {
            // Format: [type(1)][sender_id(4)][frame_number(4)][timestamp(4)][flags(1)][w(2)][h(2)][codec(1)][data(N)]
            if (len < 19) return;  // 1 + 4 + 14 minimum
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            on_video_frame_received(sender_id, data + 5, len - 5);
        }
        else if (type == parties::protocol::STREAM_AUDIO_PACKET_TYPE) {
            // Format: [type(1)][sender_id(4)][opus_data(N)]
            if (len < 6) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            if (sender_id == viewing_sharer_)
                stream_audio_player_.push_packet(data + 5, len - 5);
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
        if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
        stream_audio_player_.clear();
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

    // Register custom elements before loading documents
    video_instancer_ = std::make_unique<VideoElementInstancer>();
    Rml::Factory::RegisterElementInstancer("video_frame", video_instancer_.get());
    level_meter_instancer_ = std::make_unique<LevelMeterInstancer>();
    Rml::Factory::RegisterElementInstancer("level_meter", level_meter_instancer_.get());

    // Load UI document (single merged layout)
    doc_ = ui_.load_document("ui/lobby.rml");
    if (doc_) {
        ui_.show_document(doc_);
        level_meter_ = static_cast<LevelMeterElement*>(doc_->GetElementById("voice-level-meter"));
    }

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
    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    stream_audio_player_.shutdown();
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    if (encoder_ && encode_registered_) {
        encoder_->unregister_inputs();
    }
    for (auto& t : encode_textures_) t.Reset();
    encode_registered_ = false;
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    audio_.shutdown();
    net_.disconnect();
    ui_.shutdown();
    flush_pending_prefs(true);
    settings_.close();
}

void App::update() {
	ZoneScopedN("App::update");
    // Check if captured window was closed
    if (capture_lost_.exchange(false, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[App] Capture target lost, stopping screen share\n");
        stop_screen_share();
    }

    // Check async connection progress
    if (awaiting_connection_)
        poll_connecting();

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

    // ESC exits fullscreen stream view
    if (model_.stream_fullscreen && (GetAsyncKeyState(VK_ESCAPE) & 1)) {
        model_.stream_fullscreen = false;
        model_.dirty("stream_fullscreen");
    }

    // Sync OS window fullscreen state with model
    if (ui_.is_fullscreen() != model_.stream_fullscreen)
        ui_.set_fullscreen(model_.stream_fullscreen);

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

    // Deliver latest decoded video frame to VideoElement for GPU rendering.
    if (new_frame_available_ && doc_) {
        ZoneScopedN("App::deliver_video_frame");
        std::vector<uint8_t> y, u, v;
        uint32_t w = 0, h = 0, ys = 0, uvs = 0;
        bool nv12 = false;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (new_frame_available_) {
                y.swap(shared_y_);
                u.swap(shared_u_);
                v.swap(shared_v_);
                w = shared_width_;
                h = shared_height_;
                ys = shared_y_stride_;
                uvs = shared_uv_stride_;
                nv12 = shared_nv12_;
                new_frame_available_ = false;
            }
        }
        if (!y.empty() && w > 0 && h > 0) {
            stream_frame_count_.fetch_add(1, std::memory_order_relaxed);
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) {
                auto* ve = static_cast<VideoElement*>(elem);
                if (nv12)
                    ve->UpdateNV12Frame(y, ys, u, uvs, w, h);
                else
                    ve->UpdateYUVFrame(y.data(), ys, u.data(), v.data(), uvs, w, h);
            }
        }
        // Return spent buffers so they cycle back to staging_ via swap chain.
        // Without this, buffers get destroyed and staging_ must malloc every frame.
        // Only return if no new frame arrived in the meantime.
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (!new_frame_available_) {
                shared_y_.swap(y);
                shared_u_.swap(u);
                shared_v_.swap(v);
            }
        }
    }

    // Flush debounced preference saves
    flush_pending_prefs();

    // Update voice level meter
    update_voice_level();

    // Update speaking indicators
    update_speaking_state();

    // Update FPS counter in titlebar (once per second)
    fps_frame_count_++;
    auto now_fps = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now_fps - fps_last_update_).count();
    if (elapsed >= 1.0f) {
        int fps = static_cast<int>(fps_frame_count_ / elapsed);
        fps_frame_count_ = 0;
        fps_last_update_ = now_fps;
        if (doc_) {
            if (auto* elem = doc_->GetElementById("titlebar-fps"))
                elem->SetInnerRML(Rml::String(std::to_string(fps) + " fps"));
        }

        // Update stream FPS (encode or decode)
        uint32_t sc = stream_frame_count_.exchange(0, std::memory_order_relaxed);
        int sfps = static_cast<int>(sc / elapsed);
        if (sfps != model_.stream_fps) {
            model_.stream_fps = sfps;
            model_.dirty("stream_fps");
        }
    }

    // Update + render UI
    ui_.update();
    ui_.render_begin();
    ui_.render_body();
    ui_.render_end();
}

void App::update_voice_level() {
	ZoneScopedN("App::update_voice_level");
    if (!level_meter_ || !model_.is_connected) return;

    float level = audio_.voice_level();
    level_meter_->SetLevel(level);
    level_meter_->SetThreshold(model_.vad_threshold);
}

void App::update_speaking_state() {
    if (!model_.is_connected || current_channel_ == 0) return;

    auto now = std::chrono::steady_clock::now();
    auto levels = mixer_.get_user_levels();
    bool changed = false;

    // Local user: speaking if mic level is above threshold and not muted
    bool self_active = !model_.is_muted && audio_.voice_level() > 0.001f;
    if (self_active)
        voice_last_active_[user_id_] = now;

    for (auto& ch : model_.channels) {
        for (auto& user : ch.users) {
            bool was_speaking = user.speaking;
            UserId uid = static_cast<UserId>(user.id);

            bool active_now;
            if (uid == user_id_) {
                active_now = self_active;
            } else {
                auto it = levels.find(uid);
                active_now = (it != levels.end() && it->second > 0.001f);
            }

            // Muted/deafened users never show as speaking
            if (user.muted || user.deafened)
                active_now = false;

            if (active_now) {
                voice_last_active_[uid] = now;
                user.speaking = true;
            } else {
                // Hold for 200ms before clearing to prevent blinking
                auto last_it = voice_last_active_.find(uid);
                if (last_it != voice_last_active_.end()) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_it->second).count();
                    user.speaking = (elapsed < 200);
                } else {
                    user.speaking = false;
                }
            }

            if (user.speaking != was_speaking)
                changed = true;
        }
    }

    if (changed)
        model_.dirty("channels");
}

void App::apply_user_audio_prefs(UserId user_id) {
    auto prefix = "user." + std::to_string(user_id);

    auto vol_str = settings_.get_pref(prefix + ".volume");
    if (vol_str) {
        float vol = std::strtof(vol_str->c_str(), nullptr);
        mixer_.set_user_volume(user_id, vol);
    }

    auto comp_str = settings_.get_pref(prefix + ".compress");
    if (comp_str) {
        bool enabled = (*comp_str == "1");
        float target = 0.8f;
        auto target_str = settings_.get_pref(prefix + ".compress_target");
        if (target_str)
            target = std::strtof(target_str->c_str(), nullptr);
        mixer_.set_user_compression(user_id, enabled, target);
    }
}

void App::save_pref_debounced(const std::string& key, std::string value) {
    pending_prefs_[key] = {std::move(value), std::chrono::steady_clock::now()};
}

void App::flush_pending_prefs(bool force) {
    if (pending_prefs_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    for (auto it = pending_prefs_.begin(); it != pending_prefs_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.updated).count();
        if (force || age >= 500) {
            settings_.set_pref(it->first, it->second.value);
            it = pending_prefs_.erase(it);
        } else {
            ++it;
        }
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

    // Already connected — go straight to auth
    if (net_.is_connected()) {
        finish_connect();
        return;
    }

    // Already connecting — don't start again
    if (net_.is_connecting()) return;

    server_model_.login_status = "Connecting...";
    server_model_.dirty("login_status");

    // Start async QUIC connection
    auto ticket = settings_.load_resumption_ticket(server_host_, server_port_);
    if (!net_.connect(server_host_, server_port_,
                      ticket.empty() ? nullptr : ticket.data(), ticket.size())) {
        server_model_.login_error = "Failed to connect to server";
        server_model_.dirty("login_error");
        return;
    }
    awaiting_connection_ = true;
    // poll_connecting() in update() will handle the rest
}

void App::poll_connecting() {
    if (net_.connect_failed()) {
        awaiting_connection_ = false;
        server_model_.login_error = "Failed to connect to server";
        server_model_.login_status = "";
        server_model_.dirty("login_error");
        server_model_.dirty("login_status");
        net_.disconnect();  // Clean up QUIC resources
        return;
    }

    if (net_.is_connected()) {
        awaiting_connection_ = false;
        finish_connect();
    }
}

void App::finish_connect() {
    // TOFU check
    std::string fp = net_.get_server_fingerprint();
    auto result = settings_.check_fingerprint(server_host_, server_port_, fp);
    if (result == Settings::TofuResult::Mismatch) {
        server_model_.tofu_fingerprint = Rml::String(fp);
        server_model_.show_tofu_warning = true;
        server_model_.show_login = false;
        server_model_.dirty("tofu_fingerprint");
        server_model_.dirty("show_tofu_warning");
        server_model_.dirty("show_login");
        return;
    }
    if (result == Settings::TofuResult::Unknown) {
        settings_.trust_fingerprint(server_host_, server_port_, fp);
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

    awaiting_channel_join_ = true;
    pending_channel_id_ = id;

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

    // Remove ourselves from the channel we're leaving
    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(current_channel_)) {
            auto& u = ch.users;
            u.erase(std::remove_if(u.begin(), u.end(),
                [this](const ChannelUser& cu) { return cu.id == static_cast<int>(user_id_); }),
                u.end());
            ch.user_count = static_cast<int>(u.size());
            break;
        }
    }

    current_channel_ = 0;
    channel_key_ = {};
    sound_player_.play(SoundPlayer::Effect::LeaveChannel);
    audio_.stop();
    mixer_.clear();

    model_.current_channel = 0;
    model_.current_channel_name.clear();
    model_.dirty("current_channel");
    model_.dirty("current_channel_name");
    model_.dirty("channels");
}

// ═══════════════════════════════════════════════════════════════════════
// Server message processing
// ═══════════════════════════════════════════════════════════════════════

void App::process_server_messages() {
	ZoneScopedN("App::process_server_messages");
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
        case protocol::ControlMessageType::USER_VOICE_STATE:
            on_user_voice_state(msg.payload.data(), msg.payload.size());
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

    // Save existing user lists so we can preserve them
    std::unordered_map<int, Rml::Vector<ChannelUser>> old_users;
    for (auto& ch : model_.channels)
        old_users[ch.id] = std::move(ch.users);

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

        // Restore previous user list if available
        auto it = old_users.find(ch.id);
        if (it != old_users.end()) {
            ch.users = std::move(it->second);
            ch.user_count = static_cast<int>(ch.users.size());
        }

        model_.channels.push_back(std::move(ch));
    }

    model_.dirty("channels");
}

void App::on_channel_user_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    ChannelId channel_id = reader.read_u32();
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    // Parse users
    std::vector<ChannelUser> users;
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
        users.push_back(std::move(user));
    }

    // Populate the channel's user list
    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ch.users = std::move(users);
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }

    // If this is the channel we're joining, do the full join flow
    if (awaiting_channel_join_ && pending_channel_id_ == channel_id) {
        awaiting_channel_join_ = false;

        // Remove ourselves from the old channel's user list
        if (current_channel_ != 0 && current_channel_ != channel_id) {
            for (auto& ch : model_.channels) {
                if (ch.id == static_cast<int>(current_channel_)) {
                    auto& u = ch.users;
                    u.erase(std::remove_if(u.begin(), u.end(),
                        [this](const ChannelUser& cu) { return cu.id == static_cast<int>(user_id_); }),
                        u.end());
                    ch.user_count = static_cast<int>(u.size());
                    break;
                }
            }
        }

        current_channel_ = channel_id;
        model_.current_channel = static_cast<int>(channel_id);
        for (auto& ch : model_.channels) {
            if (ch.id == static_cast<int>(channel_id)) {
                model_.current_channel_name = ch.name;
                // Apply saved per-user audio prefs for everyone in the channel
                for (auto& u : ch.users) {
                    if (static_cast<uint32_t>(u.id) != user_id_)
                        apply_user_audio_prefs(static_cast<UserId>(u.id));
                }
                break;
            }
        }
        model_.dirty("current_channel");
        model_.dirty("current_channel_name");
        audio_.start();
        sound_player_.play(SoundPlayer::Effect::JoinChannel);
    }

    model_.dirty("channels");
}

void App::on_user_joined(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t uid = reader.read_u32();
    std::string uname = reader.read_string();
    uint32_t channel_id = reader.read_u32();
    uint8_t urole = reader.has_remaining(1) ? reader.read_u8() : 3;
    if (reader.error()) return;

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ChannelUser user;
            user.id = static_cast<int>(uid);
            user.name = Rml::String(uname);
            user.role = urole;
            ch.users.push_back(std::move(user));
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }

    if (channel_id == current_channel_) {
        sound_player_.play(SoundPlayer::Effect::UserJoined);
        apply_user_audio_prefs(uid);
    }

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

void App::send_voice_state() {
    if (!authenticated_ || current_channel_ == 0) return;
    BinaryWriter writer;
    writer.write_u8(model_.is_muted ? 1 : 0);
    writer.write_u8(model_.is_deafened ? 1 : 0);
    net_.send_message(protocol::ControlMessageType::VOICE_STATE_UPDATE,
                     writer.data().data(), writer.data().size());
}

void App::on_user_voice_state(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t uid = reader.read_u32();
    uint8_t muted = reader.read_u8();
    uint8_t deafened = reader.read_u8();
    if (reader.error()) return;

    for (auto& ch : model_.channels) {
        for (auto& user : ch.users) {
            if (user.id == static_cast<int>(uid)) {
                user.muted = (muted != 0);
                user.deafened = (deafened != 0);
            }
        }
    }
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
	ZoneScopedN("App::show_share_picker");
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
	ZoneScopedN("App::start_screen_share");
    if (sharing_screen_ || !authenticated_ || current_channel_ == 0) return;

    if (target_index < 0 || target_index >= static_cast<int>(capture_targets_.size())) {
        capture_targets_.clear();
        if (capture_) { capture_->shutdown(); capture_.reset(); }
        return;
    }

    // capture_ was already initialized in show_share_picker()
    if (!capture_) return;

    const auto& target = capture_targets_[target_index];

    // Get process ID for window-specific audio capture
    uint32_t target_process_id = 0;
    if (target.type == CaptureTarget::Type::Window && target.handle) {
        DWORD pid = 0;
        GetWindowThreadProcessId(static_cast<HWND>(target.handle), &pid);
        target_process_id = static_cast<uint32_t>(pid);
    }

    // Map FPS preset index to actual FPS
    constexpr uint32_t fps_presets[] = {15, 30, 60, 120};
    int fps_idx = (std::max)(0, (std::min)(model_.share_fps, 3));
    encode_fps_ = fps_presets[fps_idx];

    if (!capture_->start(target, encode_fps_)) {
        std::fprintf(stderr, "[App] Failed to start capture\n");
        capture_->shutdown();
        capture_.reset();
        capture_targets_.clear();
        return;
    }

    capture_->on_closed = [this]() {
        capture_lost_.store(true, std::memory_order_relaxed);
    };

    capture_targets_.clear();

    // Save share settings
    settings_.set_pref("video.share_bitrate", std::to_string(model_.share_bitrate));
    settings_.set_pref("video.share_fps", std::to_string(model_.share_fps));

    video_frame_number_ = 0;

    // Initialize capture timing for frame rate limiting
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    qpc_frequency_ = freq.QuadPart;
    capture_start_qpc_ = now.QuadPart;
    last_capture_qpc_ = 0;
    capture_interval_qpc_ = freq.QuadPart / encode_fps_;

    // Encoder-to-network callback (shared across encoder reinits)
    stream_frame_count_.store(0, std::memory_order_relaxed);
    auto on_encoded_cb = [this](const uint8_t* data, size_t len, bool keyframe) {
        if (!sharing_screen_ || !authenticated_ || !encoder_) return;
        stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

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
            if (awaiting_keyframe_) {
                if (!keyframe) return;
                awaiting_keyframe_ = false;
            }
            std::lock_guard<std::mutex> lock(decode_queue_mutex_);
            decode_queue_.push({std::vector<uint8_t>(data, data + len), 0});
            decode_queue_cv_.notify_one();
        }
    };

    // Store callback for encode thread (codec auto-selects AV1 > H.265 > H.264)
    encode_on_encoded_ = on_encoded_cb;

    // Initialize triple-buffer slot state
    encode_write_slot_ = 0;
    encode_ready_slot_ = -1;
    encode_active_slot_ = -1;
    encode_tex_w_ = 0;
    encode_tex_h_ = 0;
    encode_registered_ = false;
    for (int i = 0; i < ENCODE_SLOTS; i++) {
        encode_textures_[i].Reset();
        encode_nvenc_slots_[i] = -1;
    }

    // Start encode thread before wiring capture callback
    encode_running_.store(true, std::memory_order_release);
    encode_thread_ = std::thread([this] { encode_loop(); });

    // Wire capture -> encode thread.
    // Capture callback: rate-limit, CopyResource to free slot (fast GPU cmd), signal.
    // Encode thread picks latest slot, encodes directly via NVENC registered input (zero-copy).
    capture_->on_frame = [this](ID3D11Texture2D* texture, uint32_t w, uint32_t h) {
        ZoneScopedN("capture::on_frame");
        if (!sharing_screen_) return;

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        // Skip tiny frames (window not yet fully initialized / transitioning)
        if (desc.Width < 64 || desc.Height < 64)
            return;

        // Frame rate limiting: skip frames that arrive faster than target FPS
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t elapsed = now.QuadPart - last_capture_qpc_;
        if (elapsed < capture_interval_qpc_)
            return;
        last_capture_qpc_ = now.QuadPart;

        // Even-align dimensions for encoder
        uint32_t tex_w = (desc.Width + 1) & ~1u;
        uint32_t tex_h = (desc.Height + 1) & ~1u;

        // Recreate all staging textures if dimensions changed
        if (encode_tex_w_ != tex_w || encode_tex_h_ != tex_h) {
            // Wait for encode thread to finish current frame before recreating textures
            std::unique_lock<std::mutex> lock(encode_mutex_);
            encode_cv_.wait(lock, [this] { return encode_active_slot_ < 0; });

            // Unregister old NVENC inputs (encoder reinit will re-register)
            if (encoder_ && encode_registered_) {
                encoder_->unregister_inputs();
                encode_registered_ = false;
            }
            for (int i = 0; i < ENCODE_SLOTS; i++) {
                encode_nvenc_slots_[i] = -1;
            }

            D3D11_TEXTURE2D_DESC staging_desc{};
            staging_desc.Width = tex_w;
            staging_desc.Height = tex_h;
            staging_desc.MipLevels = 1;
            staging_desc.ArraySize = 1;
            staging_desc.Format = desc.Format;
            staging_desc.SampleDesc.Count = 1;
            staging_desc.Usage = D3D11_USAGE_DEFAULT;
            staging_desc.BindFlags = 0;

            for (int i = 0; i < ENCODE_SLOTS; i++) {
                encode_textures_[i].Reset();
                HRESULT hr = capture_->device()->CreateTexture2D(
                    &staging_desc, nullptr, &encode_textures_[i]);
                if (FAILED(hr)) {
                    std::fprintf(stderr, "[App] Failed to create staging texture %d: 0x%08lx\n", i, hr);
                    return;
                }
            }
            encode_tex_w_ = tex_w;
            encode_tex_h_ = tex_h;
            encode_write_slot_ = 0;
            encode_ready_slot_ = -1;
            // encode_active_slot_ is -1 because we hold the mutex
        }

        // Pick the pre-computed free write slot
        int ws;
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            ws = encode_write_slot_;
        }

        // GPU copy to staging texture + flush to ensure the command is submitted
        // before NVENC tries to read it on the encode thread. Without Flush(),
        // the D3D11 command buffer accumulates indefinitely (no sync point on
        // this thread), and NVENC's nvEncMapInputResource can't see pending copies.
        {
            ZoneScopedN("capture::CopyResource");
            capture_->context()->CopyResource(encode_textures_[ws].Get(), texture);
            capture_->context()->Flush();
        }

        // Publish this slot as ready and pre-compute next free write slot
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            encode_ready_slot_ = ws;
            encode_ready_ts_ = (now.QuadPart - capture_start_qpc_) * 10'000'000LL / qpc_frequency_;

            // Next write slot: not ready and not active
            for (int i = 0; i < ENCODE_SLOTS; i++) {
                if (i != encode_ready_slot_ && i != encode_active_slot_) {
                    encode_write_slot_ = i;
                    break;
                }
            }
        }
        encode_cv_.notify_one();
    };

    // Start loopback audio capture (process-specific for windows, system-wide for monitors)
    stream_audio_capture_ = std::make_unique<StreamAudioCapture>();
    if (stream_audio_capture_->init(target_process_id)) {
        stream_audio_capture_->on_encoded_frame = [this](const uint8_t* data, size_t len) {
            if (!sharing_screen_ || !authenticated_) return;

            // Packet: [STREAM_AUDIO_PACKET_TYPE(1)][opus_data(N)]
            std::vector<uint8_t> pkt(1 + len);
            pkt[0] = protocol::STREAM_AUDIO_PACKET_TYPE;
            std::memcpy(pkt.data() + 1, data, len);
            net_.send_data(pkt.data(), pkt.size());
        };
        stream_audio_capture_->start();
    } else {
        std::fprintf(stderr, "[App] Loopback audio capture unavailable (continuing without)\n");
        stream_audio_capture_.reset();
    }

    // Send SCREEN_SHARE_START to server (don't set sharing_screen_ until confirmed)
    // Encoder isn't created yet (lazy init on first frame), send AV1 placeholder and 0x0 dims.
    // Actual codec/resolution is in each video frame header.
    BinaryWriter writer;
    writer.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
    writer.write_u16(0);
    writer.write_u16(0);
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_START,
                      writer.data().data(), writer.data().size());
}

void App::stop_screen_share() {
	ZoneScopedN("App::stop_screen_share");
    if (!sharing_screen_) return;

    sharing_screen_ = false;

    // Stop self-preview
    if (viewing_sharer_ == user_id_) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
        viewing_sharer_ = 0;
    }

    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }

    // Stop encode thread before destroying encoder (thread may be mid-encode)
    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    // Unregister NVENC inputs BEFORE freeing staging textures —
    // NVENC may not AddRef the D3D11 textures on registration.
    if (encoder_ && encode_registered_) {
        encoder_->unregister_inputs();
    }
    for (auto& t : encode_textures_) t.Reset();
    encode_tex_w_ = 0;
    encode_tex_h_ = 0;
    encode_registered_ = false;
    for (auto& s : encode_nvenc_slots_) s = -1;
    encode_on_encoded_ = nullptr;

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
	ZoneScopedN("App::on_video_frame_received");
    // data = [frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec_id(1)][encoded(N)]
    if (len < 14) return;
    if (sender_id != viewing_sharer_) return;

    uint32_t frame_number;
    std::memcpy(&frame_number, data, 4);
    int64_t timestamp = static_cast<int64_t>(frame_number);
    uint8_t flags = data[8];
    bool is_keyframe = (flags & VIDEO_FLAG_KEYFRAME) != 0;

    // Skip non-keyframes until the first keyframe after a stream switch
    if (awaiting_keyframe_) {
        if (!is_keyframe) return;
        awaiting_keyframe_ = false;
    }

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
	ZoneScopedN("App::on_screen_share_started");
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

        // Use codec/dimensions from server message (authoritative, avoids null encoder_ race)
        auto codec = static_cast<VideoCodecId>(codec_id);

        // Add self to active sharers (appears as a tab)
        SharerInfo self_info;
        self_info.user_id = user_id_;
        self_info.name = username_;
        self_info.codec = codec;
        self_info.width = width;
        self_info.height = height;
        active_sharers_[user_id_] = std::move(self_info);

        // Only start self-preview if not already watching someone else
        if (viewing_sharer_ == 0) {
            viewing_sharer_ = user_id_;
            decoder_ = std::make_unique<VideoDecoder>();
            if (decoder_->init(codec, width, height)) {
                awaiting_keyframe_ = true;
                if (encoder_) encoder_->force_keyframe();
                start_decode_thread();
            }
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
	ZoneScopedN("App::on_screen_share_stopped");
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
    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    capture_targets_.clear();
    video_frame_number_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-sharer subscribe / stream switching
// ═══════════════════════════════════════════════════════════════════════

void App::watch_sharer(UserId id) {
	ZoneScopedN("App::watch_sharer");
    auto it = active_sharers_.find(id);
    if (it == active_sharers_.end() || id == viewing_sharer_) return;

    // Stop current decode if switching
    if (viewing_sharer_ != 0) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        stream_audio_player_.clear();

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
        // Self-preview: force a keyframe so the decoder can start immediately.
        awaiting_keyframe_ = true;
        if (encoder_) encoder_->force_keyframe();
    } else {
        // Remote sharer: subscribe via server and request keyframe.
        // Skip P-frames until the keyframe arrives.
        awaiting_keyframe_ = true;
        BinaryWriter writer;
        writer.write_u32(id);
        net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                          writer.data().data(), writer.data().size());
        send_pli(id);
    }

    sync_sharer_model();
}

void App::stop_watching() {
	ZoneScopedN("App::stop_watching");
    if (viewing_sharer_ == 0) return;

    bool was_self = (viewing_sharer_ == user_id_);

    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    stream_audio_player_.clear();

    if (doc_) {
        auto* elem = doc_->GetElementById("screen-share");
        if (elem) static_cast<VideoElement*>(elem)->Clear();
    }

    viewing_sharer_ = 0;
    model_.stream_fullscreen = false;

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
    stream_audio_player_.clear();

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
// Video encode thread — runs blocking NVENC encode off the capture callback
// ═══════════════════════════════════════════════════════════════════════

void App::encode_loop() {
    ZoneScopedN("App::encode_loop");
    TracySetThreadName("VideoEncode");

    while (encode_running_.load(std::memory_order_relaxed)) {
        int slot = -1;
        int64_t ts = 0;

        // Wait for a ready frame, then claim it
        {
            ZoneScopedN("encode::wait");
            std::unique_lock<std::mutex> lock(encode_mutex_);
            encode_cv_.wait(lock, [this] {
                return encode_ready_slot_ >= 0 ||
                       !encode_running_.load(std::memory_order_relaxed);
            });

            if (!encode_running_.load(std::memory_order_relaxed)) break;
            if (encode_ready_slot_ < 0) continue;

            slot = encode_ready_slot_;
            ts = encode_ready_ts_;
            encode_ready_slot_ = -1;
            encode_active_slot_ = slot;
        }

        // Check if encoder needs (re)creation — dimensions changed
        uint32_t w = encode_tex_w_;
        uint32_t h = encode_tex_h_;

        if (!encoder_ || w != encoder_->width() || h != encoder_->height()) {
            VideoCodecId codec = encoder_ ? encoder_->codec() : VideoCodecId::AV1;
            encoder_.reset();
            encode_registered_ = false;
            auto enc = std::make_unique<VideoEncoder>();
            uint32_t bitrate_bps = static_cast<uint32_t>(model_.share_bitrate * 1'000'000.0f);
            bitrate_bps = (std::max)(bitrate_bps, VIDEO_MIN_BITRATE);
            bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
            if (!enc->init(capture_->device(), w, h, 0, 0, encode_fps_, bitrate_bps, codec)) {
                std::fprintf(stderr, "[App] Encoder init failed at %ux%u\n", w, h);
                {
                    std::lock_guard<std::mutex> lock(encode_mutex_);
                    encode_active_slot_ = -1;
                }
                encode_cv_.notify_one();
                continue;
            }
            enc->on_encoded = encode_on_encoded_;
            encoder_ = std::move(enc);
            video_frame_number_ = 0;

            // Register all staging textures for zero-copy NVENC encode
            if (encoder_->supports_registered_input()) {
                bool ok = true;
                for (int i = 0; i < ENCODE_SLOTS; i++) {
                    if (encode_textures_[i]) {
                        encode_nvenc_slots_[i] = encoder_->register_input(encode_textures_[i].Get());
                        if (encode_nvenc_slots_[i] < 0) { ok = false; break; }
                    }
                }
                encode_registered_ = ok;
            }
        }

        // Encode — zero-copy if NVENC registered, otherwise fallback to CopyResource path
        {
            ZoneScopedN("encode::frame");
            if (encode_registered_ && encode_nvenc_slots_[slot] >= 0) {
                encoder_->encode_registered(encode_nvenc_slots_[slot], ts);
            } else {
                encoder_->encode_frame(encode_textures_[slot].Get(), ts);
            }
        }

        // Release slot back to free pool + notify (dimension change may be waiting)
        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            encode_active_slot_ = -1;
        }
        encode_cv_.notify_one();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Video decode thread
// ═══════════════════════════════════════════════════════════════════════

void App::start_decode_thread() {
	ZoneScopedN("App::start_decode_thread");
    if (decode_running_) return;

    // Set up decoder callback: copy I420 planes into shared buffers.
    // GPU does the YUV→RGB conversion in a pixel shader — zero CPU conversion.
    decoder_->on_decoded = [this](const DecodedFrame& frame) {
        ZoneScopedN("on_decoded::copy_planes");
        uint32_t w = frame.width, h = frame.height;
        uint32_t half_h = h / 2;

        // Reuse staging buffers — resize is free after first frame (same size).
        // memcpy from pinned GPU memory is the only real cost (~1ms for 4K NV12).
        {
            ZoneScopedN("copy_planes::Y");
            size_t y_size = static_cast<size_t>(frame.y_stride) * h;
            staging_y_.resize(y_size);
            std::memcpy(staging_y_.data(), frame.y_plane, y_size);
        }

        size_t uv_size = static_cast<size_t>(frame.uv_stride) * half_h;
        {
            ZoneScopedN("copy_planes::U");
            staging_u_.resize(uv_size);
            std::memcpy(staging_u_.data(), frame.u_plane, uv_size);
        }
        

        if (!frame.nv12 && frame.v_plane) {
            ZoneScopedN("copy_planes::V");
            staging_v_.resize(uv_size);
            std::memcpy(staging_v_.data(), frame.v_plane, uv_size);
        }

        // Swap under lock — pointer exchanges only (~0 ns).
        // Old shared_ buffers move into staging_, reused next frame.
        {
            ZoneScopedN("copy_planes::swap");
            std::lock_guard<std::mutex> lock(frame_mutex_);
            shared_y_.swap(staging_y_);
            shared_u_.swap(staging_u_);
            shared_v_.swap(staging_v_);
            shared_width_ = w;
            shared_height_ = h;
            shared_y_stride_ = frame.y_stride;
            shared_uv_stride_ = frame.uv_stride;
            shared_nv12_ = frame.nv12;
            new_frame_available_ = true;
        }
        
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
	TracySetThreadName("VideoDecoder");
    // If the decode queue backs up beyond this many frames, the decoder
    // can't keep up.  Flush everything and request a keyframe (PLI) so
    // the sharer sends a fresh IDR/key and we restart cleanly.
    static constexpr size_t MAX_DECODE_QUEUE = 10;

    while (decode_running_) {
        ZoneScopedN("App::decode_loop");
        std::queue<DecodeWork> batch;
        {
            ZoneScopedN("decode_loop::wait");
            std::unique_lock<std::mutex> lock(decode_queue_mutex_);
            decode_queue_cv_.wait(lock, [this]() {
                return !decode_queue_.empty() || !decode_running_;
            });
            if (!decode_running_) break;
            batch.swap(decode_queue_);
        }

        // If queue is too deep, decoder can't keep up — drop everything
        // and request a keyframe so we restart from a clean reference.
        if (batch.size() > MAX_DECODE_QUEUE) {
            std::fprintf(stderr, "[Video] Decode queue backed up (%zu frames), "
                         "flushing and requesting keyframe\n", batch.size());
            if (decoder_) decoder_->flush();
            while (!batch.empty()) batch.pop();

            if (viewing_sharer_ != 0)
                send_pli(viewing_sharer_);
            continue;
        }

        // Decode every frame (inter-predicted codecs need all frames
        // for correct reference chains).  The on_decoded callback
        // overwrites the shared buffer each time, so the main thread
        // only sees the latest decoded result.
        while (!batch.empty()) {
            auto& work = batch.front();
            if (decoder_ && !work.data.empty()) {
                decoder_->decode(work.data.data(), work.data.size(), work.timestamp);

                // GPU context lost (e.g., game launch) — reinitialize with software decoder
                if (decoder_->context_lost()) {
                    std::fprintf(stderr, "[App] NVDEC context lost, falling back to software decoder\n");
                    auto codec = decoder_->codec();
                    auto cb = decoder_->on_decoded;
                    decoder_->shutdown();
                    decoder_->init(codec, 0, 0);  // Reinit — will fall back to dav1d/MFT
                    decoder_->on_decoded = cb;
                    // Request keyframe to restart clean
                    if (viewing_sharer_ != 0) send_pli(viewing_sharer_);
                    while (!batch.empty()) batch.pop();
                    break;
                }
            }
            batch.pop();
            if (!decode_running_) break;
        }
    }
}

} // namespace parties::client
