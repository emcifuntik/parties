#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>

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

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    // Initialize D3D11 device for capture
    bool init();
    void shutdown();

    // Enumerate available capture targets
    std::vector<CaptureTarget> enumerate_windows();
    std::vector<CaptureTarget> enumerate_monitors();

    // Start capturing the given target at the desired FPS
    bool start(const CaptureTarget& target, uint32_t target_fps = 60);
    void stop();

    bool is_capturing() const { return capturing_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Get the D3D11 device (for sharing with encoder)
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }

    // Callback when a new frame is captured.
    // The texture is only valid for the duration of the callback.
    // You must copy it if you need it longer.
    std::function<void(ID3D11Texture2D* texture, uint32_t width, uint32_t height)> on_frame;

    // Called when the captured window/monitor is closed or lost.
    std::function<void()> on_closed;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    bool capturing_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frame_count_ = 0;

    // WinRT capture state (pimpl to avoid WinRT headers in this header)
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace parties::client
