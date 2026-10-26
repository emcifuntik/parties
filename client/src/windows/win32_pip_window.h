#pragma once

#include <client/pip_geometry.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace parties::client {

class Win32PipWindow {
public:
    struct Callbacks {
        std::function<bool(int x, int y)> is_action_at;
        std::function<bool(int x, int y)> takes_keyboard_at;
        std::function<bool(UINT message, WPARAM w_param, LPARAM l_param)> on_input;
        std::function<void(int width, int height, float scale)> on_resized;
        std::function<void(const PipRect& rect)> on_geometry_changed;
    };

    static constexpr wchar_t kClassName[] = L"PartiesPictureInPicture";
    static constexpr int kResizeBorderDp = 6;

    Win32PipWindow() = default;
    ~Win32PipWindow();
    Win32PipWindow(const Win32PipWindow&) = delete;
    Win32PipWindow& operator=(const Win32PipWindow&) = delete;

    bool create(Callbacks callbacks);
    void show(const PipRect& rect);
    void hide();
    void destroy();

    HWND hwnd() const { return hwnd_; }
    bool visible() const { return visible_.load(std::memory_order_acquire); }
    PipRect window_rect() const;

    void set_aspect(double aspect);
    double aspect() const { return aspect_.load(std::memory_order_acquire); }
    bool request_size(int width, int height) const;
    bool cursor_inside() const;
    bool interacting() const {
        return pointer_captured_.load(std::memory_order_acquire) ||
               active_.load(std::memory_order_acquire);
    }

    static std::vector<PipRect> visible_work_areas();
    static PipRect placement(const std::optional<PipRect>& remembered, double aspect,
                             const std::vector<PipRect>& work_areas, float scale);

    static void apply_aspect_to_sizing(RECT& rect, WPARAM edge, double aspect, int min_width);

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT handle(UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT hit_test(LPARAM l_param) const;
    float scale() const;
    void report_size();

    HWND hwnd_ = nullptr;
    Callbacks callbacks_;
    std::atomic<bool> visible_{false};
    std::atomic<bool> pointer_captured_{false};
    std::atomic<bool> active_{false};
    bool releasing_capture_ = false;
    std::atomic<double> aspect_{kPipDefaultAspect};
};

}
