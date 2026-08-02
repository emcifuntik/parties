#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

// Owns transient frameless HWND surfaces backed by independent RmlUi contexts.
// Only one context window is active at a time, matching native context-menu
// semantics while allowing rich controls such as sliders and switches.
class ContextWindowManager {
public:
    struct Action {
        int id = 0;
        std::string label;
        std::string detail;
        bool danger = false;
        bool separator = false;
    };

    struct UserRequest {
        int user_id = 0;
        std::string name;
        std::string channel_name;
        int role = 3;
        bool can_manage_roles = false;
        bool can_kick = false;
        float volume = 1.0f;
        float music_volume = 1.0f;
        bool compression = false;
        float compression_target = 0.8f;
    };

    struct UserCallbacks {
        std::function<void(float)> set_volume;
        std::function<void(float)> set_music_volume;
        std::function<void(bool, float)> set_compression;
        std::function<void(int)> set_role;
        std::function<void()> kick;
    };

    struct ActionRequest {
        std::string title;
        std::string subtitle;
        std::string icon_text;
        bool room_icon = false;
        std::vector<Action> actions;
        int width_dp = 310;
    };

    ContextWindowManager();
    ~ContextWindowManager();

    ContextWindowManager(const ContextWindowManager&) = delete;
    ContextWindowManager& operator=(const ContextWindowManager&) = delete;

    bool init(HWND owner, std::recursive_mutex* ui_mutex);
    // Remove the popup context while RmlUi is still alive, retaining the
    // renderer until the application's global Rml::Shutdown has completed.
    void prepare_shutdown();
    void shutdown();

    void show_user(const UserRequest& request, UserCallbacks callbacks);
    void show_actions(const ActionRequest& request, std::function<void(int)> on_action);

    // Called by App's render thread after the main window frame is presented.
    void render();
    void close();
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parties::client
