#pragma once

#include <client/rml_binding.h>

#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

namespace rml = parties::rml;

struct ServerEntry {
    int id = 0;
    Rml::String name;
    Rml::String host;
    int port = 7800;
    Rml::String last_username;
    Rml::String initials;   // first 2 chars of name, computed by App
    int color_index = 0;    // 0-4, derived from name hash for icon color

    // Live status from the connectionless server query (auto-refreshed while
    // the lobby is visible). Populated by AppCore's polling thread.
    bool        online = false;
    Rml::String users_text;     // "3 / 64" when online, empty otherwise
    bool        locked = false; // password-protected
};

class ServerListModel : public rml::Model {
public:
    // --- Bound state (Property<T> auto-dirties on assignment) ---
    rml::Prop<Rml::Vector<ServerEntry>> servers;
    rml::Prop<Rml::String> party_count_text;  // e.g. "2 parties"

    // Global profile name shown on the server launcher. This is independent
    // from the optional nickname override stored for each saved server.
    rml::Prop<Rml::String> global_name;
    rml::Prop<bool>        show_global_name_editor{false};
    rml::Prop<Rml::String> global_name_input;
    rml::Prop<Rml::String> global_name_error;

    // Per-server nickname editor, opened from the saved server context menu.
    // An empty input resolves to `global_name` when saved/connected.
    rml::Prop<bool>        show_server_nickname_editor{false};
    rml::Prop<int>         server_nickname_server_id{0};
    rml::Prop<Rml::String> server_nickname_server_name;
    rml::Prop<Rml::String> server_nickname_input;
    rml::Prop<Rml::String> server_nickname_error;

    // Add server form. An empty nickname resolves to the global profile name.
    rml::Prop<bool>        show_add_form{false};
    rml::Prop<Rml::String> edit_host;
    rml::Prop<Rml::String> edit_port;
    rml::Prop<Rml::String> edit_nickname;
    rml::Prop<Rml::String> edit_error;

    // Login overlay
    rml::Prop<bool>        show_login{false};
    rml::Prop<bool>        login_show_username{false};
    rml::Prop<Rml::String> login_username;
    rml::Prop<Rml::String> login_password;
    rml::Prop<Rml::String> login_error;
    rml::Prop<Rml::String> login_status;

    // Identity / seed phrase onboarding
    rml::Prop<bool>        show_onboarding{false};
    rml::Prop<int>         onboarding_step{0};  // 0=path selection, 1=recovery confirmation
    rml::Prop<bool>        show_restore{false};
    rml::Prop<bool>        show_key_import{false};
    rml::Prop<Rml::String> seed_phrase;
    rml::Prop<Rml::String> restore_phrase;
    rml::Prop<Rml::String> import_key_hex;
    rml::Prop<Rml::String> fingerprint;
    rml::Prop<bool>        has_identity{false};

    // Active server tracking
    rml::Prop<int>         connected_server_id{0};

    // Auto-reconnect banner (shown in the lobby while reconnecting)
    rml::Prop<bool>        reconnecting{false};
    rml::Prop<Rml::String> reconnect_status;

    // TOFU certificate warning
    rml::Prop<bool>        show_tofu_warning{false};
    rml::Prop<Rml::String> tofu_fingerprint;  // new (mismatched) fingerprint

    // --- Callbacks (set by App) ---
    std::function<void(int)>  on_connect_server;
    std::function<void(int)>  on_delete_server;
    std::function<void()>     on_save_global_name;
    std::function<void(int)>  on_edit_server_nickname;
    std::function<void()>     on_save_server_nickname;
    std::function<void()>     on_save_server;
    std::function<void()>     on_do_connect;
    std::function<void()>     on_cancel_login;
    std::function<void()>     on_generate_identity;
    std::function<void()>     on_save_identity;
    std::function<void()>     on_restore_identity;
    std::function<void()>     on_show_restore;
    std::function<void()>     on_show_key_import;
    std::function<void()>     on_import_key;
    std::function<void()>     on_copy_fingerprint;
    std::function<void()>     on_copy_seed;
    std::function<void(int)>  on_show_server_menu;  // server_id
    std::function<void()>     on_tofu_accept;       // trust new certificate
    std::function<void()>     on_tofu_reject;       // cancel connection

protected:
    const char* model_name() const override { return "serverlist"; }
    void build(rml::Builder& b) override;
};

} // namespace parties::client
