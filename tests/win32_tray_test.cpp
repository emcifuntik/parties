#include <client/win32_tray.h>

#include <cstdio>

using parties::client::Win32Tray;

namespace {

Win32Tray* active_tray = nullptr;
bool shell_available = true;
bool supports_version4 = true;
bool registered = false;
bool passed = true;
bool expected_checked = true;
int additions = 0;
int removals = 0;
int normal_closes = 0;
int menus_opened = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        passed = false;
    }
}
BOOL WINAPI notify_icon(DWORD operation, PNOTIFYICONDATAW data)
{
    if (operation == NIM_ADD) {
        ++additions;
        check(data->hWnd && data->hIcon && data->uID == Win32Tray::icon_id && data->uCallbackMessage == Win32Tray::callback_message && (data->uFlags & (NIF_MESSAGE | NIF_ICON | NIF_TIP)) == (NIF_MESSAGE | NIF_ICON | NIF_TIP), "invalid tray registration");
        registered = shell_available;
        return registered;
    }
    if (operation == NIM_DELETE) {
        ++removals;
        registered = false;
        return TRUE;
    }
    if (operation == NIM_SETVERSION) {
        check(data->uVersion == NOTIFYICON_VERSION_4, "missing version 4 negotiation");
        return registered && supports_version4;
    }
    return shell_available && registered;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (message == WM_INITMENUPOPUP) {
        ++menus_opened;
        HMENU menu = reinterpret_cast<HMENU>(w_param);
        check(GetMenuItemCount(menu) == 5, "tray menu is missing actions");
        check(GetMenuDefaultItem(menu, FALSE, 0) == Win32Tray::Show,
            "Show Parties is not the default action");
        const UINT state = GetMenuState(menu, Win32Tray::ToggleMode, MF_BYCOMMAND);
        check(state != static_cast<UINT>(-1) && ((state & MF_CHECKED) != 0) == expected_checked,
            "tray menu checkmark does not match the saved mode");
    }
    if (active_tray && active_tray->handle_message(message, w_param, l_param))
        return 0;
    if (message == WM_CLOSE) {
        ++normal_closes;
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

void command(HWND hwnd, Win32Tray::Command command)
{
    SendMessageW(hwnd, WM_COMMAND, command, 0);
}

void activate(HWND hwnd, UINT event, UINT id = Win32Tray::icon_id)
{
    SendMessageW(hwnd, Win32Tray::callback_message, MAKELPARAM(20, 20), MAKELPARAM(event, id));
}

}

int main()
{
    const auto instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"PartiesTrayTest";
    WNDCLASSW window_class { };
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class))
        return 1;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"Tray lifecycle test",
        WS_OVERLAPPEDWINDOW, 0, 0, 240, 160, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
        return 1;

    Win32Tray tray(notify_icon);
    active_tray = &tray;
    bool saved_mode = true;
    bool save_succeeds = true;
    int saved_changes = 0;
    const auto save_mode = [&](bool enabled) {
        if (!save_succeeds)
            return false;
        saved_mode = enabled;
        ++saved_changes;
        return true;
    };
    int menu_ticks = 0;
    const auto menu_tick = [&] {
        ++menu_ticks;
        EndMenu();
    };
    check(tray.init(hwnd, saved_mode, save_mode, menu_tick), "tray initialization failed");
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(!IsWindowVisible(hwnd) && IsWindow(hwnd) && normal_closes == 0,
        "close-to-tray did not hide the live window");
    activate(hwnd, NIN_SELECT, Win32Tray::icon_id + 1);
    check(!IsWindowVisible(hwnd), "an unrelated tray notification restored the window");
    activate(hwnd, NIN_KEYSELECT);
    check(IsWindowVisible(hwnd), "keyboard activation did not restore the window");

    ShowWindow(hwnd, SW_MAXIMIZE);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    command(hwnd, Win32Tray::Show);
    check(IsWindowVisible(hwnd) && IsZoomed(hwnd), "restore lost maximized placement");
    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_MINIMIZE);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    activate(hwnd, NIN_SELECT);
    check(IsWindowVisible(hwnd) && !IsIconic(hwnd), "restore left the window minimized");

    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    activate(hwnd, WM_CONTEXTMENU);
    check(menus_opened == 1 && menu_ticks > 0 && !IsWindowVisible(hwnd),
        "hidden tray menu did not keep the logic tick running");
    command(hwnd, Win32Tray::ToggleMode);
    check(!saved_mode && saved_changes == 1 && IsWindowVisible(hwnd),
        "disabling tray mode did not persist and restore the window");
    check(registered, "disabling tray mode removed the way to re-enable it");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(normal_closes == 1 && IsWindowVisible(hwnd), "disabled tray mode swallowed close");
    expected_checked = false;
    activate(hwnd, WM_CONTEXTMENU);
    check(menus_opened == 2, "disabled tray menu was unavailable");

    tray.shutdown();
    check(tray.init(hwnd, saved_mode, save_mode, menu_tick), "tray reinitialization failed");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(normal_closes == 2, "saved disabled mode was ignored on initialization");
    command(hwnd, Win32Tray::ToggleMode);
    check(saved_mode && saved_changes == 2, "tray mode could not be re-enabled");
    save_succeeds = false;
    command(hwnd, Win32Tray::ToggleMode);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(!IsWindowVisible(hwnd) && saved_mode && saved_changes == 2,
        "failed preference save changed tray behavior");
    save_succeeds = true;

    const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    const int previous_additions = additions;
    registered = false;
    SendMessageW(hwnd, taskbar_created, 0, 0);
    check(registered && additions == previous_additions + 1 && !IsWindowVisible(hwnd),
        "Explorer restart did not replace the tray icon");
    shell_available = false;
    registered = false;
    SendMessageW(hwnd, taskbar_created, 0, 0);
    check(IsWindowVisible(hwnd), "failed Explorer recovery stranded the hidden window");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(normal_closes == 3 && IsWindowVisible(hwnd), "missing tray icon swallowed close");
    shell_available = true;
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(registered && !IsWindowVisible(hwnd), "tray registration could not recover on close");

    command(hwnd, Win32Tray::Quit);
    MSG message { };
    bool quit_received = false;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            quit_received = message.wParam == 0;
            break;
        }
        DispatchMessageW(&message);
    }
    check(quit_received, "Quit did not post normal application shutdown");
    check(!registered, "Quit left the notification icon registered");
    const int previous_removals = removals;
    tray.shutdown();
    tray.shutdown();
    check(removals == previous_removals, "tray cleanup is not idempotent");

    supports_version4 = false;
    check(tray.init(hwnd, true, save_mode), "legacy shell initialization failed");
    SendMessageW(hwnd, Win32Tray::callback_message, Win32Tray::icon_id, WM_LBUTTONUP);
    check(IsWindowVisible(hwnd), "legacy shell activation failed");
    tray.shutdown();
    shell_available = false;
    check(!tray.init(hwnd, true, save_mode), "unavailable shell initialization reported success");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(normal_closes == 4, "initial tray failure swallowed close");
    shell_available = true;
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(!IsWindowVisible(hwnd), "initial tray failure could not recover");
    tray.shutdown();

    active_tray = nullptr;
    DestroyWindow(hwnd);
    UnregisterClassW(class_name, instance);
    return passed ? 0 : 1;
}
