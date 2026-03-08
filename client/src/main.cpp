#include <client/app.h>
#include <parties/version.h>
#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>

#include "RmlUi_Platform_Win32.h"
#include "dx12/RmlUi_Renderer_DX12.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <RmlUi/Debugger.h>

#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

using namespace parties::client;

// ═══════════════════════════════════════════════════════════════════════
// Window-action keyword indices (must match RCSS registration order
// in ui_manager.cpp: "none, caption, close, minimize, maximize")
// ═══════════════════════════════════════════════════════════════════════

enum WindowAction {
    WA_NONE     = 0,
    WA_CAPTION  = 1,
    WA_CLOSE    = 2,
    WA_MINIMIZE = 3,
    WA_MAXIMIZE = 4,
};

static constexpr int RESIZE_BORDER_PX = 8;
static bool s_was_minimized = false;
static bool s_in_sizemove = false;

// ═══════════════════════════════════════════════════════════════════════
// WndProc — custom titlebar, resize borders, RmlUi input forwarding
// ═══════════════════════════════════════════════════════════════════════

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetPropW(hwnd, L"App"));
    auto* ui = app ? app->ui_manager() : nullptr;

    switch (msg) {
    case WM_CLOSE:
        // Don't let DefWindowProcW call DestroyWindow — we do cleanup in main()
        PostQuitMessage(0);
        return 0;

    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            // Collapse non-client area for borderless window.
            // When maximized, constrain to the monitor's work area.
            if (IsZoomed(hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(mon, &mi);
                params->rgrc[0] = mi.rcWork;
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        if (!ui) break;

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        bool maximized = IsZoomed(hwnd);
        int border = maximized ? 0 : RESIZE_BORDER_PX;

        // Resize borders (not when maximized)
        if (!maximized) {
            bool top    = pt.y < border;
            bool bottom = pt.y >= h - border;
            bool left   = pt.x < border;
            bool right  = pt.x >= w - border;

            if (top && left)     return HTTOPLEFT;
            if (top && right)    return HTTOPRIGHT;
            if (bottom && left)  return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top)    return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left)   return HTLEFT;
            if (right)  return HTRIGHT;
        }

        // Query RmlUi element under cursor for window-action property
        auto* ctx = ui->context();
        if (ctx) {
            auto* elem = ctx->GetElementAtPoint(
                Rml::Vector2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
            if (elem) {
                const auto* prop = elem->GetProperty("window-action");
                if (prop && prop->unit == Rml::Unit::KEYWORD) {
                    switch (prop->value.Get<int>()) {
                    case WA_CAPTION:  return HTCAPTION;
                    case WA_CLOSE:    return HTCLOSE;
                    case WA_MINIMIZE: return HTMINBUTTON;
                    case WA_MAXIMIZE: return HTMAXBUTTON;
                    }
                }
            }
        }

        return HTCLIENT;
    }

    // Forward non-client mouse movement to RmlUi so :hover works on titlebar
    case WM_NCMOUSEMOVE: {
        if (!ui || !ui->context()) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        ui->context()->ProcessMouseMove(pt.x, pt.y, 0);
        break;
    }

    // DefWindowProc button tracking doesn't work in borderless windows.
    // Swallow the down event, perform the action on button-up.
    case WM_NCLBUTTONDOWN:
        if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)
            return 0;
        break;

    case WM_NCLBUTTONUP:
        if (!ui) break;
        switch (wParam) {
        case HTMINBUTTON: ui->minimize_window(); return 0;
        case HTMAXBUTTON: ui->toggle_maximize(); return 0;
        case HTCLOSE:     ui->close_window();    return 0;
        }
        break;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
        mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
        mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
        mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        mmi->ptMinTrackSize.x = 800;
        mmi->ptMinTrackSize.y = 600;
        return 0;
    }

    // Prevent GDI from painting over the DX12 swapchain
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // Keep app running during the modal move/resize loop.
    // DefWindowProcW blocks our main loop, so we render from WM_MOVING/WM_SIZING
    // which fire on every mouse move during drag. WM_TIMER is too low priority
    // (only dispatched when the queue is empty, which rarely happens during drag).
    case WM_ENTERSIZEMOVE:
        s_in_sizemove = true;
        if (ui) ui->dx12_renderer()->SetInSizeMove(true);
        return 0;
    case WM_EXITSIZEMOVE:
        s_in_sizemove = false;
        if (ui) ui->dx12_renderer()->SetInSizeMove(false);
        return 0;
    case WM_MOVING:
    case WM_SIZING:
        if (app) app->update();
        break;  // let DefWindowProc handle the actual move/size

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (wParam == SIZE_MINIMIZED) {
            s_was_minimized = true;
            if (ui) ui->on_minimize();
        } else if (ui) {
            bool restoring = s_was_minimized;
            s_was_minimized = false;
            ui->on_resize(w, h);
            if (restoring) {
                // Force DWM to re-register the window surface immediately
                // (before the next render) to avoid a white frame flash.
                MARGINS margins = {0, 0, 0, 1};
                DwmExtendFrameIntoClientArea(hwnd, &margins);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        if (ui) {
            UINT dpi = HIWORD(wParam);
            ui->on_dpi_change(static_cast<float>(dpi) / 96.0f);
        }
        auto* rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    // F8 toggles RmlUi debugger
    case WM_KEYDOWN:
        if (wParam == VK_F8) {
            Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
            return 0;
        }
        break;
    }

    // Forward input events to RmlUi via vendored Win32 platform layer
    if (ui && ui->context()) {
        bool propagating = RmlWin32::WindowProcedure(
            ui->context(), ui->text_input_editor(), hwnd, msg, wParam, lParam);
        if (!propagating)
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════════════

int main(int /*argc*/, char* /*argv*/[]) {
    TracySetThreadName("Main");
    std::printf("%s Client v%s\n", parties::APP_NAME, parties::APP_VERSION);

    // Per-monitor DPI awareness (must be set before creating any windows)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!parties::crypto_init()) {
        std::fprintf(stderr, "Failed to initialize crypto\n");
        return 1;
    }

    if (!parties::net_init()) {
        std::fprintf(stderr, "Failed to initialize networking\n");
        parties::crypto_cleanup();
        return 1;
    }

    if (!parties::quic_init()) {
        std::fprintf(stderr, "Failed to initialize QUIC\n");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /*IDC_ARROW*/);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"PartiesClient";

    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "Failed to register window class\n");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    // Create borderless window. WS_CAPTION enables DWM minimize/restore
    // animations; WM_NCCALCSIZE collapses the caption to zero pixels.
    HWND hwnd = CreateWindowExW(
        0,
        L"PartiesClient",
        L"Parties",
        WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        std::fprintf(stderr, "Failed to create window\n");
        UnregisterClassW(L"PartiesClient", wc.hInstance);
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    // Enable DWM shadow on borderless window
    MARGINS margins = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Initialize application
    App app;
    SetPropW(hwnd, L"App", &app);

    if (!app.init(hwnd)) {
        std::fprintf(stderr, "Failed to initialize application\n");
        SetPropW(hwnd, L"App", nullptr);
        DestroyWindow(hwnd);
        UnregisterClassW(L"PartiesClient", wc.hInstance);
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);

    // Force frame recalculation now that the app is initialized.
    // WS_CAPTION style means the initial WM_NCCALCSIZE during CreateWindowExW
    // fires before the app exists, so its WM_SIZE goes to DefWindowProcW.
    // This SWP_FRAMECHANGED triggers a fresh WM_SIZE through our handler,
    // ensuring RmlUi gets the correct client dimensions for layout.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Main loop
    bool running = true;
    MSG msg{};
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // When minimized, no rendering occurs and the vsync wait is skipped,
        // so throttle the loop to avoid spinning the CPU.
        if (app.ui_manager()->is_minimized()) {
            Sleep(16);
            continue;
        }

        app.update();
    }

    // Cleanup (order matters: app before window destruction)
    SetPropW(hwnd, L"App", nullptr);
    app.shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(L"PartiesClient", wc.hInstance);
    parties::quic_cleanup();
    parties::net_cleanup();
    parties::crypto_cleanup();
    return 0;
}
