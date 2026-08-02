#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace parties::client {

// Win32 requires DestroyWindow to run on the thread that created the HWND.
// Transient RmlUI surfaces are rendered and finalized by a render thread, so
// cross-thread destruction must be posted back to the owning message queue.
class OwnerThreadWindowDestroy {
public:
    static constexpr UINT message_id = WM_APP + 0x4C;

    // Call before reading window user data in the WndProc. The manager may
    // already have completed teardown by the time this message is dispatched.
    static bool handle_message(HWND hwnd, UINT message) noexcept {
        if (message != message_id)
            return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        return true;
    }

    static void destroy(HWND hwnd) noexcept {
        if (!hwnd)
            return;

        const DWORD owner_thread = GetWindowThreadProcessId(hwnd, nullptr);
        if (owner_thread == GetCurrentThreadId()) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        } else {
            PostMessageW(hwnd, message_id, 0, 0);
        }
    }
};

// SetWindowPos may synchronously call into the HWND owner thread. Calling it
// from the render thread while holding the RmlUi mutex can therefore deadlock:
// the owner thread enters the WndProc and waits for that same mutex. Always
// marshal popup placement to the thread that owns the HWND.
class OwnerThreadWindowResize {
public:
    static constexpr UINT message_id = WM_APP + 0x4D;

    static bool handle_message(HWND hwnd, UINT message, WPARAM width_param,
                               LPARAM height_param) noexcept {
        if (message != message_id)
            return false;

        const int width = static_cast<int>(width_param);
        const int height = static_cast<int>(height_param);
        if (width <= 0 || height <= 0)
            return true;

        RECT window_rect{};
        if (!GetWindowRect(hwnd, &window_rect))
            return true;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor, &monitor_info))
            return true;

        const int work_width = monitor_info.rcWork.right - monitor_info.rcWork.left;
        const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
        const int clamped_width = (std::min)(width, work_width);
        const int clamped_height = (std::min)(height, work_height);
        const int x = (std::clamp)(window_rect.left, monitor_info.rcWork.left,
                                   monitor_info.rcWork.right - clamped_width);
        const int y = (std::clamp)(window_rect.top, monitor_info.rcWork.top,
                                   monitor_info.rcWork.bottom - clamped_height);

        SetWindowPos(hwnd, HWND_TOPMOST, x, y, clamped_width, clamped_height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return true;
    }

    static bool request(HWND hwnd, int width, int height) noexcept {
        if (!hwnd || width <= 0 || height <= 0)
            return false;

        const DWORD owner_thread = GetWindowThreadProcessId(hwnd, nullptr);
        if (owner_thread == GetCurrentThreadId())
            return handle_message(hwnd, message_id, static_cast<WPARAM>(width),
                                  static_cast<LPARAM>(height));

        return PostMessageW(hwnd, message_id, static_cast<WPARAM>(width),
                            static_cast<LPARAM>(height)) != FALSE;
    }
};

} // namespace parties::client
