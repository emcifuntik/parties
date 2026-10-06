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

// Win32 shell of the desktop picture-in-picture window: a borderless,
// always-on-top tool window that never takes activation and is not owned by
// the main window (an owned window would be hidden with a minimized owner).
// The whole surface drags the window except overlay actions; the edges resize
// it while keeping the stream's aspect ratio.
//
// Thread affinity: create/show/hide/destroy and the window procedure run on
// the thread that called create() (the application message thread).
// set_aspect, cursor_inside, request_size and destroy are safe from any
// thread. Rendering is the host's job; this class knows nothing about it.
class Win32PipWindow {
public:
    struct Callbacks {
        // Hit test: is client point (physical px) on an overlay action? Those
        // points receive clicks; everything else drags the window.
        std::function<bool(int x, int y)> is_action_at;
        // Client-area input for the hosted UI. Return true when consumed.
        std::function<bool(UINT message, WPARAM w_param, LPARAM l_param)> on_input;
        // Client size or DPI changed (physical px, scale = dpi / 96).
        std::function<void(int width, int height, float scale)> on_resized;
        // A user move/resize finished; persist this window rect.
        std::function<void(const PipRect& rect)> on_geometry_changed;
    };

    static constexpr wchar_t kClassName[] = L"PartiesPictureInPicture";
    static constexpr int kResizeBorderDp = 6;

    Win32PipWindow() = default;
    ~Win32PipWindow();
    Win32PipWindow(const Win32PipWindow&) = delete;
    Win32PipWindow& operator=(const Win32PipWindow&) = delete;

    // Creates the hidden HWND on the calling thread.
    bool create(Callbacks callbacks);
    // Show without activating at `rect` (virtual-screen px), topmost.
    void show(const PipRect& rect);
    void hide();
    // Destroys the HWND on its owner thread; safe from any thread.
    void destroy();

    HWND hwnd() const { return hwnd_; }
    bool visible() const { return visible_.load(std::memory_order_acquire); }
    PipRect window_rect() const;

    // Stream aspect ratio (width / height) enforced by interactive resizing.
    void set_aspect(double aspect);
    double aspect() const { return aspect_.load(std::memory_order_acquire); }
    // Resize to `width` x `height` px from any thread without blocking: the
    // owner thread applies it and keeps the window on its monitor.
    bool request_size(int width, int height) const;
    // True while the cursor is over the window (any thread, no messages).
    bool cursor_inside() const;

    // Work areas of all monitors, primary first (virtual-screen px).
    static std::vector<PipRect> visible_work_areas();
    // Where to open: the remembered rect clamped fully onto a visible work
    // area, or the bottom-right of the primary work area. Sized for `aspect`.
    static PipRect placement(const std::optional<PipRect>& remembered, double aspect,
                             const std::vector<PipRect>& work_areas, float scale);

    // Exposed for tests: the aspect-preserving WM_SIZING adjustment.
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
    std::atomic<double> aspect_{kPipDefaultAspect};
};

} // namespace parties::client
