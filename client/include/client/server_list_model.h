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
    Rml::String initials;
    int color_index = 0;

    bool        online = false;
    Rml::String users_text;
    bool        locked = false;
};

class ServerListModel : public rml::Model {
public:
    rml::Prop<Rml::Vector<ServerEntry>> servers;
    rml::Prop<Rml::String> party_count_text;

    rml::Prop<Rml::String> global_name;
    rml::Prop<bool>        show_global_name_editor{false};
    rml::Prop<Rml::String> global_name_input;
    rml::Prop<Rml::String> global_name_error;

    rml::Prop<bool>        show_server_nickname_editor{false};
    rml::Prop<int>         server_nickname_server_id{0};
    rml::Prop<Rml::String> server_nickname_server_name;
    rml::Prop<Rml::String> server_nickname_input;
    rml::Prop<Rml::String> server_nickname_error;

    rml::Prop<bool>        show_add_form{false};
    rml::Prop<Rml::String> edit_host;
    rml::Prop<Rml::String> edit_port;
    rml::Prop<Rml::String> edit_nickname;
    rml::Prop<Rml::String> edit_error;

    rml::Prop<bool>        show_login{false};
    rml::Prop<bool>        login_show_username{false};
    rml::Prop<Rml::String> login_username;
    rml::Prop<Rml::String> login_password;
    rml::Prop<Rml::String> login_error;
    rml::Prop<Rml::String> login_status;

    rml::Prop<bool>        show_onboarding{false};
    rml::Prop<int>         onboarding_step{0};
    rml::Prop<bool>        show_restore{false};
    rml::Prop<bool>        show_key_import{false};
    rml::Prop<Rml::String> seed_phrase;
    rml::Prop<Rml::String> restore_phrase;
    rml::Prop<Rml::String> import_key_hex;
    rml::Prop<Rml::String> fingerprint;
    rml::Prop<bool>        has_identity{false};

    rml::Prop<int>         connected_server_id{0};

    rml::Prop<bool>        reconnecting{false};
    rml::Prop<Rml::String> reconnect_status;

    rml::Prop<bool>        show_tofu_warning{false};
    rml::Prop<Rml::String> tofu_fingerprint;

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
    std::function<void(int)>  on_show_server_menu;
    std::function<void()>     on_tofu_accept;
    std::function<void()>     on_tofu_reject;

protected:
    const char* model_name() const override { return "serverlist"; }
    void build(rml::Builder& b) override;
};

}
