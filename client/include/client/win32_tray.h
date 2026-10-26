#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

#include <functional>

namespace parties::client {
class Win32Tray {
public:
    static constexpr UINT callback_message = WM_APP + 0x4E;
    static constexpr UINT icon_id = 1;
    enum Command : UINT { Show = 0x5101,
        ToggleMode,
        Quit };
    using NotifyIcon = decltype(&Shell_NotifyIconW);

    explicit Win32Tray(NotifyIcon notify_icon = Shell_NotifyIconW);
    ~Win32Tray();
    Win32Tray(const Win32Tray&) = delete;
    Win32Tray& operator=(const Win32Tray&) = delete;

    bool init(HWND hwnd, bool enabled, std::function<bool(bool)> save_mode,
        std::function<void()> menu_tick = { });
    void shutdown();
    bool handle_message(UINT message, WPARAM w_param, LPARAM l_param);

private:
    bool add_icon();
    void remove_icon();
    void show_window();
    void show_menu(POINT position);
    void run_command(UINT command);

    NotifyIcon notify_icon_;
    NOTIFYICONDATAW icon_ { };
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    UINT_PTR menu_timer_ = 0;
    bool icon_added_ = false;
    bool version4_ = false;
    bool enabled_ = true;
    bool quitting_ = false;
    std::function<bool(bool)> save_mode_;
    std::function<void()> menu_tick_;
};

}
