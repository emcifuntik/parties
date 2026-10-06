// Win32 picture-in-picture window lifecycle: an unowned, topmost, never
// activating tool window that survives a minimized or hidden main window,
// drags and resizes with the stream's aspect ratio, reopens on a visible
// monitor, and is resized and destroyed on its owner thread.

#include "win32_pip_window.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace {

using namespace parties::client;

bool passed = true;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "PiP window: %s\n", message);
        passed = false;
    }
}

void pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::yield();
    }
}

bool on_visible_monitor(HWND hwnd) {
    RECT window{};
    GetWindowRect(hwnd, &window);
    HMONITOR monitor = MonitorFromRect(&window, MONITOR_DEFAULTTONULL);
    if (!monitor) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;
    return window.left >= work.left && window.top >= work.top &&
           window.right <= work.right && window.bottom <= work.bottom;
}

LRESULT hit(HWND hwnd, int client_x, int client_y) {
    POINT point{client_x, client_y};
    ClientToScreen(hwnd, &point);
    return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

LRESULT CALLBACK main_window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD owner_thread = GetCurrentThreadId();

    // Stand-in for the application's main window.
    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.hInstance = GetModuleHandleW(nullptr);
    main_class.lpfnWndProc = main_window_proc;
    main_class.lpszClassName = L"PartiesPipLifecycleMain";
    RegisterClassExW(&main_class);
    HWND main_window = CreateWindowExW(0, main_class.lpszClassName, L"Main", WS_OVERLAPPEDWINDOW,
        100, 100, 640, 480, nullptr, nullptr, main_class.hInstance, nullptr);
    if (!main_window) {
        std::fprintf(stderr, "failed to create the main test window\n");
        return 1;
    }
    ShowWindow(main_window, SW_SHOWNOACTIVATE);

    std::atomic<bool> action_under_cursor{false};
    std::atomic<int> resized_width{0};
    std::atomic<int> resized_height{0};
    std::atomic<DWORD> resize_thread{0};
    std::atomic<int> geometry_reports{0};
    PipRect reported_geometry;

    Win32PipWindow::Callbacks callbacks;
    callbacks.is_action_at = [&](int, int) { return action_under_cursor.load(); };
    callbacks.on_resized = [&](int width, int height, float) {
        resized_width = width;
        resized_height = height;
        resize_thread = GetCurrentThreadId();
    };
    callbacks.on_geometry_changed = [&](const PipRect& rect) {
        reported_geometry = rect;
        ++geometry_reports;
    };

    auto window = std::make_unique<Win32PipWindow>();
    check(window->create(std::move(callbacks)), "create failed");
    HWND hwnd = window->hwnd();
    check(hwnd && !IsWindowVisible(hwnd) && !window->visible(), "window was not created hidden");

    // Window kind: borderless popup, topmost tool window, never activates,
    // and not owned by any window (owned windows hide with a minimized owner).
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    check((style & WS_POPUP) && !(style & WS_CAPTION) && !(style & WS_THICKFRAME),
        "window has chrome");
    check((ex_style & WS_EX_TOPMOST) && (ex_style & WS_EX_TOOLWINDOW) && (ex_style & WS_EX_NOACTIVATE),
        "window is not a topmost, non-activating tool window");
    check(GetWindow(hwnd, GW_OWNER) == nullptr, "window has an owner");

    // A remembered rect from a monitor that no longer exists opens fully on a
    // visible monitor, at the stream's aspect ratio.
    const auto areas = Win32PipWindow::visible_work_areas();
    check(!areas.empty(), "no work areas reported");
    const PipRect stranded{-200000, -200000, 480, 270};
    const PipRect placed = Win32PipWindow::placement(stranded, 16.0 / 9.0, areas, 1.0f);
    window->show(placed);
    check(window->visible() && IsWindowVisible(hwnd), "show did not show the window");
    check(on_visible_monitor(hwnd), "remembered rect was not placed on a visible monitor");
    check(GetActiveWindow() != hwnd && GetForegroundWindow() != hwnd, "showing PiP activated it");
    check(resize_thread.load() == owner_thread && resized_width.load() > 0, "client size was not reported");

    // A remembered rect that is fully visible is used exactly; none at all
    // opens in the bottom-right corner of the primary work area.
    const PipRect primary = areas.front();
    const PipRect kept{primary.x + 40, primary.y + 40, 400, 225};
    check(Win32PipWindow::placement(kept, 16.0 / 9.0, areas, 1.0f) == kept,
        "visible remembered rect was moved");
    const PipRect fresh = Win32PipWindow::placement(std::nullopt, 16.0 / 9.0, areas, 1.0f);
    check(fresh.x + fresh.width <= primary.x + primary.width &&
          fresh.y + fresh.height <= primary.y + primary.height &&
          fresh.x > primary.x + primary.width / 2, "first placement is not bottom-right");

    // It stays up while the main window is minimized or hidden to the tray.
    ShowWindow(main_window, SW_MINIMIZE);
    pump_until([] { return false; }, std::chrono::milliseconds(50));
    check(IsWindowVisible(hwnd), "PiP hid with the minimized main window");
    ShowWindow(main_window, SW_HIDE);
    pump_until([] { return false; }, std::chrono::milliseconds(50));
    check(IsWindowVisible(hwnd), "PiP hid with the hidden main window");

    // Hit testing: edges resize, actions click, everything else drags.
    RECT client{};
    GetClientRect(hwnd, &client);
    check(hit(hwnd, 1, client.bottom / 2) == HTLEFT, "left edge does not resize");
    check(hit(hwnd, client.right - 1, client.bottom - 1) == HTBOTTOMRIGHT, "corner does not resize");
    check(hit(hwnd, client.right / 2, 1) == HTTOP, "top edge does not resize");
    check(hit(hwnd, client.right / 2, client.bottom / 2) == HTCAPTION, "surface does not drag");
    action_under_cursor = true;
    check(hit(hwnd, client.right / 2, client.bottom / 2) == HTCLIENT, "overlay action is not clickable");
    action_under_cursor = false;
    check(SendMessageW(hwnd, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(hwnd),
                       MAKELPARAM(HTCAPTION, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
        "clicking PiP activates it");

    // Interactive resizing keeps the stream's aspect ratio from every edge.
    window->set_aspect(16.0 / 9.0);
    RECT sizing{100, 100, 500, 150};
    SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&sizing));
    check(sizing.left == 100 && sizing.top == 100 && sizing.right == 500 && sizing.bottom == 325,
        "right-edge resize lost the aspect ratio");
    RECT top_left{100, 100, 500, 400};
    Win32PipWindow::apply_aspect_to_sizing(top_left, WMSZ_TOPLEFT, 16.0 / 9.0, 192);
    check(top_left.right == 500 && top_left.bottom == 400 && top_left.left == 100 &&
          top_left.top == 400 - 225, "top-left resize did not anchor the opposite corner");
    RECT bottom{100, 100, 500, 280};
    Win32PipWindow::apply_aspect_to_sizing(bottom, WMSZ_BOTTOM, 16.0 / 9.0, 192);
    check(bottom.bottom == 280 && bottom.right == 100 + 320, "bottom-edge resize is not height-driven");
    RECT tiny{0, 0, 20, 20};
    Win32PipWindow::apply_aspect_to_sizing(tiny, WMSZ_RIGHT, 16.0 / 9.0, 192);
    check(tiny.right == 192 && tiny.bottom == 108, "resize went below the minimum size");

    // The end of a user move/resize reports the rect to remember.
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    const PipRect current = window->window_rect();
    check(geometry_reports.load() == 1 && reported_geometry == current,
        "end of move did not report the window rect");

    // The render thread resizes without blocking; the owner thread applies it
    // and keeps the window on its monitor.
    std::atomic<bool> requested{false};
    std::thread render_thread([&] { requested = window->request_size(320, 180); });
    pump_until([&] { return requested.load(); }, std::chrono::milliseconds(500));
    render_thread.join();
    check(requested.load(), "cross-thread resize request failed");
    pump_until([&] { return resized_width.load() == 320; }, std::chrono::seconds(2));
    check(resized_width.load() == 320 && resized_height.load() == 180 &&
          resize_thread.load() == owner_thread, "cross-thread resize was not applied on the owner thread");
    check(on_visible_monitor(hwnd) && IsWindowVisible(hwnd), "resize moved PiP off its monitor or hid it");

    // Close and reopen reuse the same window.
    window->hide();
    check(!window->visible() && !IsWindowVisible(hwnd) && !window->cursor_inside(), "hide left PiP visible");
    window->show(window->window_rect());
    check(window->hwnd() == hwnd && IsWindowVisible(hwnd), "reopen did not reuse the window");

    // Teardown from the render thread is posted to the owner thread, and the
    // object may be gone before the message is dispatched.
    std::thread teardown([&] { window->destroy(); });
    teardown.join();
    window.reset();
    pump_until([&] { return !IsWindow(hwnd); }, std::chrono::seconds(2));
    check(!IsWindow(hwnd), "owner thread did not destroy the posted window");

    DestroyWindow(main_window);
    return passed ? 0 : 1;
}
