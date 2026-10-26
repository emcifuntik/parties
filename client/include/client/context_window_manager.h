#pragma once

#include <client/rml_shutdown.h>
#include <parties/permissions.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

typedef struct HWND__* HWND;

namespace parties::client {

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
        Role role = Role::User;
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
        std::function<void(Role)> set_role;
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
    void release_documents();
    void destroy_window(const RmlShutdownProof&);

    void show_user(const UserRequest& request, UserCallbacks callbacks);
    void show_actions(const ActionRequest& request, std::function<void(int)> on_action);

    void render();
    void close();
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
