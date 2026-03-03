#pragma once

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

// ── Data structs (mirrored from Windows client/include/client/lobby_model.h) ─

struct ChannelUser {
    Rml::String name;
    int id        = 0;
    int role      = 0;   // parties::Role cast to int
    bool muted     = false;
    bool deafened  = false;
    bool is_sharing = false;  // currently sharing their screen
};

struct ActiveSharer {
    int         id = 0;
    Rml::String name;
};

struct ChannelInfo {
    int id              = 0;
    Rml::String name;
    int user_count      = 0;
    int max_users       = 0;
    unsigned int sort_order = 0;    // not bound to DataModel
    Rml::Vector<ChannelUser> users;
};

// ── SavedServer (sidebar server entries) ─────────────────────────────────────

struct SavedServer {
    int         idx      = 0;     // index in saved_servers vector (for callbacks)
    Rml::String display_name;     // shown in tooltip (host:port)
    Rml::String initials;         // 1-2 chars shown in circle
    Rml::String host;
    Rml::String port;
    Rml::String username;
    bool        is_active = false; // currently connected to this server
};

// ── LobbyModel ────────────────────────────────────────────────────────────────
//
// Owns all RmlUi DataModel bindings for the "lobby" model.
// Call setup() once after the document has been loaded.
// Call mark_dirty() whenever bound state changes so RmlUi re-reads it.

class LobbyModel {
public:
    LobbyModel();
    ~LobbyModel();

    void setup(Rml::Context* context);
    void mark_dirty();

    // ── Login / connection ─────────────────────────────────────────────────
    Rml::String login_host     = "localhost";
    Rml::String login_port     = "7800";
    Rml::String login_username;
    Rml::String login_password;
    Rml::String login_status;
    Rml::String login_error;
    bool        show_login     = true;

    // ── Connected state ────────────────────────────────────────────────────
    bool        is_connected   = false;
    Rml::String server_name;
    Rml::String username;

    Rml::Vector<ChannelInfo> channels;
    int         current_channel      = 0;
    Rml::String current_channel_name;

    bool is_muted        = false;
    bool is_deafened     = false;
    bool show_settings   = false;
    bool denoise_enabled = true;

    // ── Saved servers sidebar ──────────────────────────────────────────────
    Rml::Vector<SavedServer> saved_servers;

    // ── Screen sharing ─────────────────────────────────────────────────────
    Rml::Vector<ActiveSharer> sharers;       // users currently sharing in channel
    int                       viewing_sharer_id = 0;  // 0 = not watching anyone

    // ── Callbacks set by PartiesViewController before setup() ─────────────
    std::function<void()>         on_connect;
    std::function<void()>         on_register;
    std::function<void(uint32_t)> on_join_channel;
    std::function<void()>         on_leave_channel;
    std::function<void()>         on_toggle_mute;
    std::function<void()>         on_toggle_deafen;
    std::function<void()>         on_toggle_settings;
    std::function<void()>         on_toggle_denoise;
    std::function<void()>         on_disconnect;
    std::function<void(int)>      on_select_server;   // arg = idx
    std::function<void()>         on_add_server;
    std::function<void(int)>      on_watch_sharer;    // arg = user_id
    std::function<void()>         on_stop_watching;

private:
    Rml::DataModelHandle handle_;
};

} // namespace parties::client
