#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#include <wrl/client.h>

namespace parties::client {

struct CaptureTarget {
    enum class Type { Window, Monitor };
    Type type;
    std::string name;
    // Opaque handles — cast back when starting capture
    void* handle = nullptr;  // HWND or HMONITOR
};

struct CaptureThumbnail {
    enum class Source { None, WindowsGraphicsCapture, Gdi };

    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    Source source = Source::None;

    explicit operator bool() const { return !rgba.empty() && width > 0 && height > 0; }
};

class ScreenCapture {
public:
    // Reusable preview-capture resources owned by the thumbnail worker. Keeping
    // this lifetime explicit is important on Windows: releasing D3D11/WinRT
    // objects from a thread-local destructor during LdrShutdownThread can
    // deadlock inside the display driver.
    class ThumbnailSession {
    public:
        ThumbnailSession();
        ~ThumbnailSession();

        ThumbnailSession(const ThumbnailSession&) = delete;
        ThumbnailSession& operator=(const ThumbnailSession&) = delete;

        CaptureThumbnail capture(const CaptureTarget& target,
                                 uint32_t max_width = 480,
                                 uint32_t max_height = 270);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    ScreenCapture();
    ~ScreenCapture();

    // Initialize D3D11 device for capture
    bool init();
    void shutdown();
    // Drain callbacks immediately, then release WGC on an independent MTA.
    // Discarding the returned future does not block the UI message loop.
    static std::future<void> shutdown_async(std::shared_ptr<ScreenCapture> capture);

    // Enumerate available capture targets
    std::vector<CaptureTarget> enumerate_windows();
    std::vector<CaptureTarget> enumerate_monitors();

    // Start capturing the given target at the desired FPS
    bool start(const CaptureTarget& target, uint32_t target_fps = 60);
    // Stop and drain frame callbacks without entering a blocking WGC RPC.
    void stop_frame_delivery();
    void stop();

    bool is_capturing() const { return capturing_.load(std::memory_order_acquire); }
    // Includes native window/monitor liveness, even if WGC never reports Closed.
    bool target_lost() const;
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Get the D3D11 device (for sharing with encoder)
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }

    // Callback when a new frame is captured.
    // The texture is only valid for the duration of the callback.
    // You must copy it if you need it longer.
    void set_frame_callback(std::function<void(ID3D11Texture2D*, uint32_t, uint32_t)> callback);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    std::atomic<bool> capturing_{false};
    std::atomic<bool> target_closed_{false};
    CaptureTarget target_{};
    uint32_t target_process_id_ = 0;
    std::function<void(ID3D11Texture2D*, uint32_t, uint32_t)> on_frame_;
    bool frame_limited_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frame_count_ = 0;
    std::chrono::steady_clock::time_point last_frame_time_{};

    // WinRT capture state (pimpl to avoid WinRT headers in this header)
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace parties::client
