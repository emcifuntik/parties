#include "app_model.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

namespace parties::client {

LobbyModel::LobbyModel()  = default;
LobbyModel::~LobbyModel() = default;

void LobbyModel::setup(Rml::Context* context)
{
    Rml::DataModelConstructor ctor = context->CreateDataModel("lobby");
    if (!ctor) return;

    // Register structs (array type must be registered before a struct that
    // contains it as a member — ChannelUser first, then ChannelInfo).
    if (auto s = ctor.RegisterStruct<ActiveSharer>()) {
        s.RegisterMember("id",   &ActiveSharer::id);
        s.RegisterMember("name", &ActiveSharer::name);
    }
    ctor.RegisterArray<Rml::Vector<ActiveSharer>>();

    if (auto s = ctor.RegisterStruct<ChannelUser>()) {
        s.RegisterMember("name",       &ChannelUser::name);
        s.RegisterMember("id",         &ChannelUser::id);
        s.RegisterMember("role",       &ChannelUser::role);
        s.RegisterMember("muted",      &ChannelUser::muted);
        s.RegisterMember("deafened",   &ChannelUser::deafened);
        s.RegisterMember("is_sharing", &ChannelUser::is_sharing);
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

    if (auto s = ctor.RegisterStruct<SavedServer>()) {
        s.RegisterMember("idx",          &SavedServer::idx);
        s.RegisterMember("display_name", &SavedServer::display_name);
        s.RegisterMember("initials",     &SavedServer::initials);
        s.RegisterMember("host",         &SavedServer::host);
        s.RegisterMember("port",         &SavedServer::port);
        s.RegisterMember("username",     &SavedServer::username);
        s.RegisterMember("is_active",    &SavedServer::is_active);
    }
    ctor.RegisterArray<Rml::Vector<SavedServer>>();

    // Login state
    ctor.Bind("login_host",     &login_host);
    ctor.Bind("login_port",     &login_port);
    ctor.Bind("login_username", &login_username);
    ctor.Bind("login_password", &login_password);
    ctor.Bind("login_status",   &login_status);
    ctor.Bind("login_error",    &login_error);
    ctor.Bind("show_login",     &show_login);

    // Connected state
    ctor.Bind("is_connected",         &is_connected);
    ctor.Bind("server_name",          &server_name);
    ctor.Bind("username",             &username);
    ctor.Bind("channels",             &channels);
    ctor.Bind("current_channel",      &current_channel);
    ctor.Bind("current_channel_name", &current_channel_name);
    ctor.Bind("is_muted",             &is_muted);
    ctor.Bind("is_deafened",          &is_deafened);
    ctor.Bind("show_settings",        &show_settings);
    ctor.Bind("denoise_enabled",      &denoise_enabled);

    // Sidebar
    ctor.Bind("saved_servers", &saved_servers);

    // Screen sharing
    ctor.Bind("sharers",           &sharers);
    ctor.Bind("viewing_sharer_id", &viewing_sharer_id);

    // Event callbacks
    ctor.BindEventCallback("do_connect",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_connect) on_connect();
        });

    ctor.BindEventCallback("do_register",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_register) on_register();
        });

    ctor.BindEventCallback("join_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_join_channel)
                on_join_channel((uint32_t)args[0].Get<int>());
        });

    ctor.BindEventCallback("leave_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_leave_channel) on_leave_channel();
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
            if (on_toggle_settings) on_toggle_settings();
        });

    ctor.BindEventCallback("toggle_denoise",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_toggle_denoise) on_toggle_denoise();
        });

    ctor.BindEventCallback("do_disconnect",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_disconnect) on_disconnect();
        });

    ctor.BindEventCallback("watch_sharer",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            fprintf(stderr, "[LobbyModel] watch_sharer event: args.size=%zu uid=%d\n",
                    args.size(), args.empty() ? -1 : args[0].Get<int>());
            if (!args.empty() && on_watch_sharer)
                on_watch_sharer(args[0].Get<int>());
        });

    ctor.BindEventCallback("stop_watching",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_stop_watching) on_stop_watching();
        });

    ctor.BindEventCallback("select_server",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_select_server)
                on_select_server(args[0].Get<int>());
        });

    ctor.BindEventCallback("add_server",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_add_server) on_add_server();
        });

    handle_ = ctor.GetModelHandle();
}

void LobbyModel::mark_dirty()
{
    if (!handle_) return;
    handle_.DirtyVariable("login_host");
    handle_.DirtyVariable("login_port");
    handle_.DirtyVariable("login_username");
    handle_.DirtyVariable("login_password");
    handle_.DirtyVariable("login_status");
    handle_.DirtyVariable("login_error");
    handle_.DirtyVariable("show_login");
    handle_.DirtyVariable("is_connected");
    handle_.DirtyVariable("server_name");
    handle_.DirtyVariable("username");
    handle_.DirtyVariable("channels");
    handle_.DirtyVariable("current_channel");
    handle_.DirtyVariable("current_channel_name");
    handle_.DirtyVariable("is_muted");
    handle_.DirtyVariable("is_deafened");
    handle_.DirtyVariable("show_settings");
    handle_.DirtyVariable("denoise_enabled");
    handle_.DirtyVariable("saved_servers");
    handle_.DirtyVariable("sharers");
    handle_.DirtyVariable("viewing_sharer_id");
}

} // namespace parties::client
