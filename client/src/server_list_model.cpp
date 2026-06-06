#include <client/server_list_model.h>

#include <RmlUi/Core/Event.h>

namespace parties::client {

void ServerListModel::build(rml::Builder& b) {
    // Register struct + array (array AFTER its element struct).
    b.register_struct<ServerEntry>([](auto& s) {
        s.member("id",            &ServerEntry::id)
         .member("name",          &ServerEntry::name)
         .member("host",          &ServerEntry::host)
         .member("port",          &ServerEntry::port)
         .member("last_username", &ServerEntry::last_username)
         .member("initials",      &ServerEntry::initials)
         .member("color_index",   &ServerEntry::color_index)
         .member("online",        &ServerEntry::online)
         .member("users_text",    &ServerEntry::users_text)
         .member("locked",        &ServerEntry::locked);
    });
    b.register_array<Rml::Vector<ServerEntry>>();

    // Bind variables.
    b.bind("servers",          servers)
     .bind("party_count_text", party_count_text)
     .bind("show_add_form",    show_add_form)
     .bind("edit_host",        edit_host)
     .bind("edit_port",        edit_port)
     .bind("edit_error",       edit_error)
     .bind("show_login",       show_login)
     .bind("login_username",   login_username)
     .bind("login_password",   login_password)
     .bind("login_error",      login_error)
     .bind("login_status",     login_status)
     .bind("connected_server_id", connected_server_id)
     .bind("show_tofu_warning", show_tofu_warning)
     .bind("tofu_fingerprint",  tofu_fingerprint)
     .bind("show_onboarding",  show_onboarding)
     .bind("show_restore",     show_restore)
     .bind("show_key_import",  show_key_import)
     .bind("seed_phrase",      seed_phrase)
     .bind("restore_phrase",   restore_phrase)
     .bind("import_key_hex",   import_key_hex)
     .bind("fingerprint",      fingerprint)
     .bind("has_identity",     has_identity);

    // Event callbacks. Property assignments inside these lambdas auto-dirty.
    b.on_event("server_mousedown", [this](Rml::Event& event, const Rml::VariantList& args) {
        if (args.empty()) return;
        int id = args[0].Get<int>();
        int button = event.GetParameter<int>("button", 0);
        if (button == 0) {
            // Left click → connect
            if (on_connect_server) on_connect_server(id);
        } else if (button == 1) {
            // Right click → native context menu
            if (on_show_server_menu) on_show_server_menu(id);
        }
    });

    b.on("add_server", [this] {
        edit_host = "";
        edit_port = "7800";
        edit_error = "";
        show_add_form = true;
    });

    b.on("save_server", [this] {
        if (on_save_server) on_save_server();
    });

    b.on("cancel_edit", [this] {
        show_add_form = false;
    });

    b.on("do_connect", [this] {
        if (on_do_connect) on_do_connect();
    });

    b.on("cancel_login", [this] {
        show_login = false;
        login_error = "";
        login_status = "";
        login_password = "";
        if (on_cancel_login) on_cancel_login();
    });

    b.on("generate_identity", [this] {
        if (on_generate_identity) on_generate_identity();
    });

    b.on("save_identity", [this] {
        if (on_save_identity) on_save_identity();
    });

    b.on("restore_identity", [this] {
        if (on_restore_identity) on_restore_identity();
    });

    b.on("show_restore", [this] {
        if (on_show_restore) on_show_restore();
    });

    b.on("show_key_import", [this] {
        if (on_show_key_import) on_show_key_import();
    });

    b.on("import_key", [this] {
        if (on_import_key) on_import_key();
    });

    b.on("copy_fingerprint", [this] {
        if (on_copy_fingerprint) on_copy_fingerprint();
    });

    b.on("copy_seed", [this] {
        if (on_copy_seed) on_copy_seed();
    });

    b.on("tofu_accept", [this] {
        if (on_tofu_accept) on_tofu_accept();
    });

    b.on("tofu_reject", [this] {
        if (on_tofu_reject) on_tofu_reject();
    });
}

} // namespace parties::client
