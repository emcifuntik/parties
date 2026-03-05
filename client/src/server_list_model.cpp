#include <client/server_list_model.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

namespace parties::client {

ServerListModel::ServerListModel() = default;
ServerListModel::~ServerListModel() = default;

bool ServerListModel::init(Rml::Context* context) {
    Rml::DataModelConstructor ctor = context->CreateDataModel("serverlist");
    if (!ctor)
        return false;

    // Register struct + array (array BEFORE any struct that uses it)
    if (auto s = ctor.RegisterStruct<ServerEntry>()) {
        s.RegisterMember("id",            &ServerEntry::id);
        s.RegisterMember("name",          &ServerEntry::name);
        s.RegisterMember("host",          &ServerEntry::host);
        s.RegisterMember("port",          &ServerEntry::port);
        s.RegisterMember("last_username", &ServerEntry::last_username);
        s.RegisterMember("initials",      &ServerEntry::initials);
    }
    ctor.RegisterArray<Rml::Vector<ServerEntry>>();

    // Bind variables
    ctor.Bind("servers",          &servers);
    ctor.Bind("show_add_form",    &show_add_form);
    ctor.Bind("editing_id",       &editing_id);
    ctor.Bind("edit_name",        &edit_name);
    ctor.Bind("edit_host",        &edit_host);
    ctor.Bind("edit_port",        &edit_port);
    ctor.Bind("edit_error",       &edit_error);
    ctor.Bind("show_login",       &show_login);
    ctor.Bind("login_username",   &login_username);
    ctor.Bind("login_error",      &login_error);
    ctor.Bind("login_status",     &login_status);
    ctor.Bind("connected_server_id", &connected_server_id);
    ctor.Bind("show_context_menu",   &show_context_menu);
    ctor.Bind("context_menu_server_id", &context_menu_server_id);

    // Identity / onboarding
    ctor.Bind("show_onboarding",  &show_onboarding);
    ctor.Bind("show_restore",     &show_restore);
    ctor.Bind("seed_phrase",      &seed_phrase);
    ctor.Bind("restore_phrase",   &restore_phrase);
    ctor.Bind("fingerprint",      &fingerprint);
    ctor.Bind("has_identity",     &has_identity);

    // Event callbacks
    ctor.BindEventCallback("server_mousedown",
        [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
            if (args.empty()) return;
            int id = args[0].Get<int>();
            int button = event.GetParameter<int>("button", 0);
            if (button == 0) {
                // Left click → connect
                if (on_connect_server) on_connect_server(id);
            } else if (button == 1) {
                // Right click → context menu
                context_menu_server_id = id;
                show_context_menu = true;
                dirty("show_context_menu");
                dirty("context_menu_server_id");
                if (on_show_context_menu)
                    on_show_context_menu(id,
                        event.GetParameter<int>("mouse_x", 0),
                        event.GetParameter<int>("mouse_y", 0));
            }
        });

    ctor.BindEventCallback("context_edit",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_context_menu = false;
            dirty("show_context_menu");
            // Populate edit form with the context menu target server
            for (auto& srv : servers) {
                if (srv.id == context_menu_server_id) {
                    editing_id = context_menu_server_id;
                    edit_name = srv.name;
                    edit_host = srv.host;
                    edit_port = Rml::String(std::to_string(srv.port));
                    edit_error = "";
                    show_add_form = true;
                    dirty("editing_id");
                    dirty("edit_name");
                    dirty("edit_host");
                    dirty("edit_port");
                    dirty("edit_error");
                    dirty("show_add_form");
                    break;
                }
            }
        });

    ctor.BindEventCallback("context_delete",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_context_menu = false;
            dirty("show_context_menu");
            if (on_delete_server)
                on_delete_server(context_menu_server_id);
        });

    ctor.BindEventCallback("dismiss_context_menu",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_context_menu = false;
            dirty("show_context_menu");
        });

    ctor.BindEventCallback("add_server",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            editing_id = 0;
            edit_name = "";
            edit_host = "";
            edit_port = "7800";
            edit_error = "";
            show_add_form = true;
            dirty("editing_id");
            dirty("edit_name");
            dirty("edit_host");
            dirty("edit_port");
            dirty("edit_error");
            dirty("show_add_form");
        });

    ctor.BindEventCallback("save_server",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_save_server) on_save_server();
        });

    ctor.BindEventCallback("cancel_edit",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_add_form = false;
            dirty("show_add_form");
        });

    ctor.BindEventCallback("do_connect",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_do_connect) on_do_connect();
        });

    ctor.BindEventCallback("cancel_login",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_login = false;
            login_error = "";
            login_status = "";
            dirty("show_login");
            dirty("login_error");
            dirty("login_status");
            if (on_cancel_login) on_cancel_login();
        });

    ctor.BindEventCallback("generate_identity",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_generate_identity) on_generate_identity();
        });

    ctor.BindEventCallback("save_identity",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_save_identity) on_save_identity();
        });

    ctor.BindEventCallback("restore_identity",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_restore_identity) on_restore_identity();
        });

    ctor.BindEventCallback("show_restore",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_show_restore) on_show_restore();
        });

    ctor.BindEventCallback("copy_fingerprint",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_copy_fingerprint) on_copy_fingerprint();
        });

    handle_ = ctor.GetModelHandle();
    return true;
}

void ServerListModel::dirty(const Rml::String& variable) {
    handle_.DirtyVariable(variable);
}

} // namespace parties::client
