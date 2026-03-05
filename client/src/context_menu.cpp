#include <client/context_menu.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace parties::client {

// ── Theme colors (match style.rcss Midnight Aurora) ──
static constexpr COLORREF kBgRaised  = RGB(0x1f, 0x20, 0x30);
static constexpr COLORREF kBgHover   = RGB(0x26, 0x27, 0x39);
static constexpr COLORREF kBorder    = RGB(0x2a, 0x2b, 0x3d);
static constexpr COLORREF kText      = RGB(0xe0, 0xdd, 0xd8);
static constexpr COLORREF kDanger    = RGB(0xe8, 0x64, 0x5a);

// ── Layout constants (base, scaled by DPI) ──
static constexpr int kItemHeight   = 32;
static constexpr int kSepHeight    = 9;  // separator line + padding
static constexpr int kPadX         = 12;
static constexpr int kPadY         = 4;  // top/bottom menu padding
static constexpr int kFontSize     = 14;

// ── Per-instance state stored in window ──
struct MenuState {
    const std::vector<ContextMenu::Item>* items;
    HFONT font;
    int hover_index;  // -1 = none
    int result;       // 0 = dismissed
    int item_h;       // scaled item height
    int sep_h;        // scaled separator height
    int pad_x;
    int pad_y;
    int width;
    int height;
    bool done;
};

static int item_y(const MenuState& s, int index) {
    int y = s.pad_y;
    for (int i = 0; i < index; i++) {
        if ((*s.items)[i].separator)
            y += s.sep_h;
        else
            y += s.item_h;
    }
    return y;
}

static int hit_test(const MenuState& s, int y) {
    int cy = s.pad_y;
    for (int i = 0; i < static_cast<int>(s.items->size()); i++) {
        int h = (*s.items)[i].separator ? s.sep_h : s.item_h;
        if (y >= cy && y < cy + h) {
            if ((*s.items)[i].separator) return -1;
            return i;
        }
        cy += h;
    }
    return -1;
}

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<MenuState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT: {
        if (!state) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Background
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg_brush = CreateSolidBrush(kBgRaised);
        FillRect(hdc, &rc, bg_brush);
        DeleteObject(bg_brush);

        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, state->font));
        SetBkMode(hdc, TRANSPARENT);

        int y = state->pad_y;
        for (int i = 0; i < static_cast<int>(state->items->size()); i++) {
            auto& item = (*state->items)[i];

            if (item.separator) {
                // Draw separator line centered vertically in sep_h
                int line_y = y + state->sep_h / 2;
                HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
                HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
                MoveToEx(hdc, state->pad_x, line_y, nullptr);
                LineTo(hdc, state->width - state->pad_x, line_y);
                SelectObject(hdc, old_pen);
                DeleteObject(pen);
                y += state->sep_h;
                continue;
            }

            // Hover highlight
            if (i == state->hover_index) {
                RECT item_rc = { 0, y, state->width, y + state->item_h };
                HBRUSH hover_brush = CreateSolidBrush(kBgHover);
                FillRect(hdc, &item_rc, hover_brush);
                DeleteObject(hover_brush);
            }

            // Text
            SetTextColor(hdc, item.danger ? kDanger : kText);
            RECT text_rc = {
                state->pad_x,
                y,
                state->width - state->pad_x,
                y + state->item_h
            };
            DrawTextW(hdc, item.label.c_str(), static_cast<int>(item.label.size()),
                      &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            y += state->item_h;
        }

        SelectObject(hdc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!state) break;
        int y = static_cast<short>(HIWORD(lParam));
        int idx = hit_test(*state, y);
        if (idx != state->hover_index) {
            state->hover_index = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Track mouse leave to dismiss when cursor exits
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (state && state->hover_index != -1) {
            state->hover_index = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        // Consume — selection happens on button up
        return 0;

    case WM_LBUTTONUP: {
        if (!state) break;
        int y = static_cast<short>(HIWORD(lParam));
        int idx = hit_test(*state, y);
        if (idx >= 0) {
            state->result = (*state->items)[idx].id;
        }
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        // Swallow right-clicks inside menu
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && state) {
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && state && !state->done) {
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool register_class() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MenuWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /*IDC_ARROW*/);
    wc.lpszClassName = L"PartiesCtxMenu";

    if (!RegisterClassExW(&wc)) return false;
    registered = true;
    return true;
}

int ContextMenu::show(HWND parent, const std::vector<Item>& items) {
    if (items.empty()) return 0;
    if (!register_class()) return 0;

    // DPI scaling
    UINT dpi = GetDpiForWindow(parent);
    float scale = static_cast<float>(dpi) / 96.0f;
    auto scaled = [scale](int v) { return static_cast<int>(v * scale + 0.5f); };

    MenuState state{};
    state.items = &items;
    state.hover_index = -1;
    state.result = 0;
    state.done = false;
    state.item_h = scaled(kItemHeight);
    state.sep_h = scaled(kSepHeight);
    state.pad_x = scaled(kPadX);
    state.pad_y = scaled(kPadY);

    // Create font
    state.font = CreateFontW(
        -scaled(kFontSize),  // negative = character height
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Noto Sans");
    if (!state.font)
        state.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // Measure items to determine popup size
    HDC screen_dc = GetDC(nullptr);
    HFONT old_font = static_cast<HFONT>(SelectObject(screen_dc, state.font));

    int max_text_w = 0;
    int total_h = state.pad_y * 2;
    for (auto& item : items) {
        if (item.separator) {
            total_h += state.sep_h;
        } else {
            SIZE sz;
            GetTextExtentPoint32W(screen_dc, item.label.c_str(),
                                   static_cast<int>(item.label.size()), &sz);
            if (sz.cx > max_text_w) max_text_w = sz.cx;
            total_h += state.item_h;
        }
    }

    SelectObject(screen_dc, old_font);
    ReleaseDC(nullptr, screen_dc);

    state.width = max_text_w + state.pad_x * 2 + scaled(16);  // extra breathing room
    state.height = total_h;

    // Position at cursor, clamp to monitor
    POINT pt;
    GetCursorPos(&pt);

    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int x = pt.x;
    int y = pt.y;
    if (x + state.width > mi.rcWork.right)
        x = mi.rcWork.right - state.width;
    if (y + state.height > mi.rcWork.bottom)
        y = mi.rcWork.bottom - state.height;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    // Create popup window
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"PartiesCtxMenu", nullptr,
        WS_POPUP,
        x, y, state.width, state.height,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) {
        if (state.font) DeleteObject(state.font);
        return 0;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    // Windows 11 rounded corners + dark border
    DWORD corner_pref = 3; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                           &corner_pref, sizeof(corner_pref));
    COLORREF border_color = 0xFFFFFFFF; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/,
                           &border_color, sizeof(border_color));

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    // Local message loop until menu closes
    MSG msg_loop;
    while (GetMessageW(&msg_loop, nullptr, 0, 0)) {
        TranslateMessage(&msg_loop);
        DispatchMessageW(&msg_loop);
    }

    if (state.font) DeleteObject(state.font);
    return state.result;
}

} // namespace parties::client
