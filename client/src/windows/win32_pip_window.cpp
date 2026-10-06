#include "win32_pip_window.h"

#include <client/win32_window_lifecycle.h>

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace parties::client {

namespace {

PipRect to_pip_rect(const RECT& rect) {
    return {static_cast<double>(rect.left), static_cast<double>(rect.top),
            static_cast<double>(rect.right - rect.left), static_cast<double>(rect.bottom - rect.top)};
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* areas = reinterpret_cast<std::vector<PipRect>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const PipRect area = to_pip_rect(info.rcWork);
    if (info.dwFlags & MONITORINFOF_PRIMARY)
        areas->insert(areas->begin(), area);
    else
        areas->push_back(area);
    return TRUE;
}

} // namespace

Win32PipWindow::~Win32PipWindow() {
    destroy();
}

bool Win32PipWindow::create(Callbacks callbacks) {
    if (hwnd_) return true;
    callbacks_ = std::move(callbacks);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &Win32PipWindow::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /*IDC_ARROW*/);
    window_class.lpszClassName = kClassName;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // No owner: Windows hides owned windows while their owner is minimized,
    // and PiP must stay up exactly then. WS_EX_NOACTIVATE keeps the window
    // from taking focus away from whatever the user is doing.
    HWND created = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"Parties picture-in-picture", WS_POPUP,
        0, 0, 384, 216, nullptr, nullptr, window_class.hInstance, this);
    hwnd_ = created;   // also set during WM_NCCREATE for creation messages
    return hwnd_ != nullptr;
}

void Win32PipWindow::show(const PipRect& rect) {
    if (!hwnd_) return;
    visible_.store(true, std::memory_order_release);
    SetWindowPos(hwnd_, HWND_TOPMOST,
        static_cast<int>(std::lround(rect.x)), static_cast<int>(std::lround(rect.y)),
        static_cast<int>(std::lround(rect.width)), static_cast<int>(std::lround(rect.height)),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    report_size();
}

void Win32PipWindow::hide() {
    if (!hwnd_) return;
    visible_.store(false, std::memory_order_release);
    ShowWindow(hwnd_, SW_HIDE);
}

void Win32PipWindow::destroy() {
    if (!hwnd_) return;
    visible_.store(false, std::memory_order_release);
    HWND hwnd = hwnd_;
    hwnd_ = nullptr;
    // Detach first: a cross-thread destroy is only posted, and messages
    // dispatched before it must not reach this (possibly destroyed) object.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    OwnerThreadWindowDestroy::destroy(hwnd);
}

PipRect Win32PipWindow::window_rect() const {
    RECT rect{};
    if (!hwnd_ || !GetWindowRect(hwnd_, &rect)) return {};
    return to_pip_rect(rect);
}

void Win32PipWindow::set_aspect(double aspect) {
    if (std::isfinite(aspect) && aspect > 0.0)
        aspect_.store(aspect, std::memory_order_release);
}

bool Win32PipWindow::request_size(int width, int height) const {
    return OwnerThreadWindowResize::request(hwnd_, width, height);
}

bool Win32PipWindow::cursor_inside() const {
    if (!hwnd_ || !visible()) return false;
    POINT cursor{};
    RECT rect{};
    return GetCursorPos(&cursor) && GetWindowRect(hwnd_, &rect) && PtInRect(&rect, cursor);
}

std::vector<PipRect> Win32PipWindow::visible_work_areas() {
    std::vector<PipRect> areas;
    EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&areas));
    return areas;
}

PipRect Win32PipWindow::placement(const std::optional<PipRect>& remembered, double aspect,
                                  const std::vector<PipRect>& work_areas, float scale) {
    PipSizeLimits limits;
    limits.min_width *= scale;
    if (remembered)
        return pip_clamp_to_work_areas(pip_fit_aspect(*remembered, aspect), work_areas, limits);
    const PipRect fallback{0.0, 0.0, 1280.0, 720.0};
    const PipRect& area = work_areas.empty() ? fallback : work_areas.front();
    return pip_default_rect(area, aspect, 384.0 * scale, 24.0 * scale);
}

void Win32PipWindow::apply_aspect_to_sizing(RECT& rect, WPARAM edge, double aspect, int min_width) {
    if (!(std::isfinite(aspect) && aspect > 0.0)) aspect = kPipDefaultAspect;
    int width = (std::max)(static_cast<int>(rect.right - rect.left), min_width);
    int height = static_cast<int>(rect.bottom - rect.top);
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        // Height-driven: the dragged edge sets the height.
        height = (std::max)(height, static_cast<int>(std::lround(min_width / aspect)));
        width = static_cast<int>(std::lround(height * aspect));
    } else {
        height = static_cast<int>(std::lround(width / aspect));
    }

    const bool anchor_right = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool anchor_bottom = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    if (anchor_right) rect.left = rect.right - width;
    else              rect.right = rect.left + width;
    if (anchor_bottom) rect.top = rect.bottom - height;
    else               rect.bottom = rect.top + height;
}

float Win32PipWindow::scale() const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return static_cast<float>(dpi ? dpi : 96) / 96.0f;
}

void Win32PipWindow::report_size() {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client) || !callbacks_.on_resized) return;
    if (client.right > 0 && client.bottom > 0)
        callbacks_.on_resized(client.right, client.bottom, scale());
}

LRESULT CALLBACK Win32PipWindow::window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    // Posted teardown and cross-thread resize run before touching the object:
    // it may already be gone when the destroy message is dispatched.
    if (OwnerThreadWindowDestroy::handle_message(hwnd, message))
        return 0;
    if (OwnerThreadWindowResize::handle_message(hwnd, message, w_param, l_param))
        return 0;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        auto* creating = static_cast<Win32PipWindow*>(create->lpCreateParams);
        creating->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating));
    }
    auto* self = reinterpret_cast<Win32PipWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || self->hwnd_ != hwnd)
        return DefWindowProcW(hwnd, message, w_param, l_param);
    return self->handle(message, w_param, l_param);
}

LRESULT Win32PipWindow::hit_test(LPARAM l_param) const {
    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    ScreenToClient(hwnd_, &point);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int border = (std::max)(1, static_cast<int>(std::lround(kResizeBorderDp * scale())));
    const bool left = point.x < border;
    const bool right = point.x >= client.right - border;
    const bool top = point.y < border;
    const bool bottom = point.y >= client.bottom - border;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    if (callbacks_.is_action_at && callbacks_.is_action_at(point.x, point.y))
        return HTCLIENT;
    return HTCAPTION;
}

LRESULT Win32PipWindow::handle(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_NCHITTEST:
        return hit_test(l_param);
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCLBUTTONDBLCLK:
        return 0;   // no maximize/restore on the drag surface
    case WM_SIZING: {
        const int min_width = static_cast<int>(std::lround(PipSizeLimits{}.min_width * scale()));
        apply_aspect_to_sizing(*reinterpret_cast<RECT*>(l_param), w_param, aspect(), min_width);
        return TRUE;
    }
    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED) report_size();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        report_size();
        return 0;
    }
    case WM_EXITSIZEMOVE:
        if (callbacks_.on_geometry_changed) callbacks_.on_geometry_changed(window_rect());
        return 0;
    case WM_CLOSE:
        return 0;   // PiP closes through its overlay or the main window only
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_NCMOUSEMOVE: {
        // The drag surface reports non-client moves; forward them as client
        // moves so overlay hover state follows the cursor off a button.
        if (callbacks_.on_input) {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ScreenToClient(hwnd_, &point);
            callbacks_.on_input(WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        // A control that needs the keyboard (the volume slider) activates the
        // window explicitly: WS_EX_NOACTIVATE only blocks click activation.
        const int x = GET_X_LPARAM(l_param);
        const int y = GET_Y_LPARAM(l_param);
        if (callbacks_.takes_keyboard_at && callbacks_.takes_keyboard_at(x, y))
            SetForegroundWindow(hwnd_);
        const bool consumed = callbacks_.on_input && callbacks_.on_input(message, w_param, l_param);
        // The hosted UI captures the mouse for the press so a slider drag
        // keeps receiving moves outside the window.
        pointer_captured_.store(GetCapture() == hwnd_, std::memory_order_release);
        if (consumed) return 0;
        break;
    }
    case WM_LBUTTONUP: {
        releasing_capture_ = true;
        const bool consumed = callbacks_.on_input && callbacks_.on_input(message, w_param, l_param);
        releasing_capture_ = false;
        pointer_captured_.store(false, std::memory_order_release);
        if (consumed) return 0;
        break;
    }
    case WM_CAPTURECHANGED:
        // Capture taken away mid-drag (another window, a system gesture): end
        // the press so the hosted UI does not keep dragging without a button.
        if (pointer_captured_.exchange(false, std::memory_order_acq_rel) && !releasing_capture_ &&
            reinterpret_cast<HWND>(l_param) != hwnd_ && callbacks_.on_input) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            releasing_capture_ = true;
            callbacks_.on_input(WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
            releasing_capture_ = false;
        }
        break;
    case WM_ACTIVATE:
        active_.store(LOWORD(w_param) != WA_INACTIVE, std::memory_order_release);
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
        // Only reaches the window after a keyboard control activated it.
        if (callbacks_.on_input && callbacks_.on_input(message, w_param, l_param))
            return 0;
        break;
    case WM_MOUSEMOVE:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSELEAVE:
        if (callbacks_.on_input && callbacks_.on_input(message, w_param, l_param))
            return 0;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, w_param, l_param);
}

} // namespace parties::client
