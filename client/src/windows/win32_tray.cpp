#include <client/win32_tray.h>

#include <windowsx.h>

#include <utility>

namespace parties::client {

Win32Tray::Win32Tray(NotifyIcon notify_icon)
    : notify_icon_(notify_icon)
{
}
Win32Tray::~Win32Tray() { shutdown(); }

bool Win32Tray::init(HWND hwnd, bool enabled, std::function<bool(bool)> save_mode,
    std::function<void()> menu_tick)
{
    shutdown();
    hwnd_ = hwnd;
    enabled_ = enabled;
    quitting_ = false;
    save_mode_ = std::move(save_mode);
    menu_tick_ = std::move(menu_tick);
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    icon_ = { };
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = hwnd_;
    icon_.uID = icon_id;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = callback_message;
    icon_.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!icon_.hIcon)
        icon_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpynW(icon_.szTip, L"Parties", ARRAYSIZE(icon_.szTip));
    return add_icon();
}

bool Win32Tray::add_icon()
{
    icon_added_ = hwnd_ && notify_icon_(NIM_ADD, &icon_);
    version4_ = false;
    if (icon_added_) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        version4_ = notify_icon_(NIM_SETVERSION, &icon_) != FALSE;
    }
    return icon_added_;
}

void Win32Tray::remove_icon()
{
    if (icon_added_)
        notify_icon_(NIM_DELETE, &icon_);
    icon_added_ = false;
}

void Win32Tray::shutdown()
{
    if (menu_timer_)
        KillTimer(hwnd_, menu_timer_);
    menu_timer_ = 0;
    remove_icon();
    hwnd_ = nullptr;
    save_mode_ = { };
    menu_tick_ = { };
}

void Win32Tray::show_window()
{
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void Win32Tray::run_command(UINT command)
{
    switch (command) {
    case Show:
        show_window();
        break;
    case ToggleMode:
        if (save_mode_ && !save_mode_(!enabled_))
            break;
        enabled_ = !enabled_;
        if (!enabled_ && !IsWindowVisible(hwnd_))
            show_window();
        break;
    case Quit:
        quitting_ = true;
        remove_icon();
        PostQuitMessage(0);
        break;
    }
}

void Win32Tray::show_menu(POINT position)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, Show, L"Show Parties");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
        ToggleMode, L"Tray mode (close to tray)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Quit, L"Quit");
    SetMenuDefaultItem(menu, Show, FALSE);
    SetForegroundWindow(hwnd_);
    if (menu_tick_)
        menu_timer_ = SetTimer(hwnd_, callback_message, 8, nullptr);
    const UINT command = TrackPopupMenuEx(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        position.x, position.y, hwnd_, nullptr);
    if (menu_timer_)
        KillTimer(hwnd_, menu_timer_);
    menu_timer_ = 0;
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (command)
        run_command(command);
    else if (icon_added_)
        notify_icon_(NIM_SETFOCUS, &icon_);
}

bool Win32Tray::handle_message(UINT message, WPARAM w_param, LPARAM l_param)
{
    if (!hwnd_ || quitting_)
        return false;
    if (taskbar_created_ && message == taskbar_created_) {
        icon_added_ = false;
        if (!add_icon() && !IsWindowVisible(hwnd_))
            show_window();
        return true;
    }
    if (message == WM_CLOSE) {
        if (!enabled_)
            return false;
        if (!icon_added_ || !notify_icon_(NIM_MODIFY, &icon_)) {
            if (!add_icon())
                return false;
        }
        ShowWindow(hwnd_, SW_HIDE);
        return true;
    }
    if (message == WM_TIMER && menu_timer_ && w_param == menu_timer_) {
        if (menu_tick_)
            menu_tick_();
        return true;
    }
    if (message == WM_COMMAND && l_param == 0 && HIWORD(w_param) == 0) {
        const UINT command = LOWORD(w_param);
        if (command == Show || command == ToggleMode || command == Quit) {
            run_command(command);
            return true;
        }
    }
    if (message != callback_message)
        return false;
    const UINT id = version4_ ? HIWORD(l_param) : static_cast<UINT>(w_param);
    if (!icon_added_ || id != icon_id)
        return true;
    const UINT event = version4_ ? LOWORD(l_param) : static_cast<UINT>(l_param);
    if (event == NIN_SELECT || event == NIN_KEYSELECT || (!version4_ && event == WM_LBUTTONUP)) {
        show_window();
    } else if (event == WM_CONTEXTMENU || (!version4_ && event == WM_RBUTTONUP)) {
        POINT position { GET_X_LPARAM(w_param), GET_Y_LPARAM(w_param) };
        if (!version4_ || (position.x == -1 && position.y == -1))
            GetCursorPos(&position);
        show_menu(position);
    }
    return true;
}

}
