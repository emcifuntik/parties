#pragma once

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

struct ServerEntry {
    int id = 0;
    Rml::String name;
    Rml::String host;
    int port = 7800;
    Rml::String last_username;
    Rml::String initials;  // first 2 chars of name, computed by App
};

class ServerListModel {
public:
    ServerListModel();
    ~ServerListModel();

    bool init(Rml::Context* context);

    void dirty(const Rml::String& variable);

    // --- Bound state ---
    Rml::Vector<ServerEntry> servers;

    // Add server form
    bool show_add_form = false;
    Rml::String edit_host;
    Rml::String edit_port;
    Rml::String edit_error;

    // Login overlay
    bool show_login = false;
    Rml::String login_username;
    Rml::String login_error;
    Rml::String login_status;

    // Identity / seed phrase onboarding
    bool show_onboarding = false;
    bool show_restore = false;
    bool show_key_import = false;
    Rml::String seed_phrase;
    Rml::String restore_phrase;
    Rml::String import_key_hex;
    Rml::String fingerprint;
    bool has_identity = false;

    // Active server tracking
    int connected_server_id = 0;

    // TOFU certificate warning
    bool show_tofu_warning = false;
    Rml::String tofu_fingerprint;  // new (mismatched) fingerprint

    // --- Callbacks (set by App) ---
    std::function<void(int)>  on_connect_server;
    std::function<void(int)>  on_delete_server;
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
    std::function<void(int)>  on_show_server_menu;  // server_id
    std::function<void()>     on_tofu_accept;       // trust new certificate
    std::function<void()>     on_tofu_reject;       // cancel connection

private:
    Rml::DataModelHandle handle_;
};

} // namespace parties::client
