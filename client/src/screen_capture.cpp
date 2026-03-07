#include <client/screen_capture.h>

// WinRT / Windows Graphics Capture headers
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dwmapi.h>
#include <dxgi.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace parties::client {

// ─── Helper: Win32 D3D11 device → WinRT IDirect3DDevice ─────────────────────

static IDirect3DDevice CreateWinRTDevice(ID3D11Device* d3dDevice) {
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

    winrt::com_ptr<::IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());

    return inspectable.as<IDirect3DDevice>();
}

// ─── Helper: create capture item ─────────────────────────────────────────────

static GraphicsCaptureItem CreateItemForWindow(HWND hwnd) {
    auto interop = winrt::get_activation_factory<
        GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        hwnd,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

static GraphicsCaptureItem CreateItemForMonitor(HMONITOR hmon) {
    auto interop = winrt::get_activation_factory<
        GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(
        hmon,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

// ─── Pimpl holding WinRT capture state ───────────────────────────────────────

struct ScreenCapture::Impl {
    IDirect3DDevice winrt_device{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool frame_pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    event_token frame_arrived_token{};
};

// ─── ScreenCapture implementation ────────────────────────────────────────────

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    shutdown();
}

bool ScreenCapture::init() {
    // Create D3D11 device with VIDEO_SUPPORT for sharing with encoder later
    D3D_FEATURE_LEVEL feature_level;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &device_, &feature_level, &context_);

    if (FAILED(hr)) {
        // Retry without VIDEO_SUPPORT
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &feature_level, &context_);
    }

    if (FAILED(hr)) {
        std::fprintf(stderr, "[ScreenCapture] Failed to create D3D11 device (0x%08lx)\n", hr);
        return false;
    }

    // Enable multithread protection (required for MFT sharing)
    ComPtr<ID3D10Multithread> mt;
    device_.As(&mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    impl_ = new Impl();

    try {
        impl_->winrt_device = CreateWinRTDevice(device_.Get());
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "[ScreenCapture] WinRT device creation failed: 0x%08x\n",
                     static_cast<unsigned>(e.code()));
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    return true;
}

void ScreenCapture::shutdown() {
    stop();
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }
    context_.Reset();
    device_.Reset();
}

std::vector<CaptureTarget> ScreenCapture::enumerate_windows() {
    std::vector<CaptureTarget> results;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto& vec = *reinterpret_cast<std::vector<CaptureTarget>*>(lParam);

        if (!IsWindowVisible(hwnd)) return TRUE;

        // Skip cloaked windows (UWP suspended, virtual desktops)
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        // Skip windows with no title
        int len = GetWindowTextLengthW(hwnd);
        if (len == 0) return TRUE;

        // Skip tool windows
        LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        // Get window title
        std::wstring title(len + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);

        // Convert to UTF-8
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string name(utf8_len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, name.data(), utf8_len, nullptr, nullptr);

        CaptureTarget target;
        target.type = CaptureTarget::Type::Window;
        target.name = std::move(name);
        target.handle = hwnd;
        vec.push_back(std::move(target));

        return TRUE;
    }, reinterpret_cast<LPARAM>(&results));

    return results;
}

std::vector<CaptureTarget> ScreenCapture::enumerate_monitors() {
    std::vector<CaptureTarget> results;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT rect, LPARAM lParam) -> BOOL {
            auto& vec = *reinterpret_cast<std::vector<CaptureTarget>*>(lParam);

            MONITORINFOEXA info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoA(hmon, &info);

            CaptureTarget target;
            target.type = CaptureTarget::Type::Monitor;
            target.name = info.szDevice;

            // Append resolution
            int w = rect->right - rect->left;
            int h = rect->bottom - rect->top;
            target.name += " (" + std::to_string(w) + "x" + std::to_string(h) + ")";

            if (info.dwFlags & MONITORINFOF_PRIMARY)
                target.name += " [Primary]";

            target.handle = hmon;
            vec.push_back(std::move(target));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&results));

    return results;
}

bool ScreenCapture::start(const CaptureTarget& target) {
    if (capturing_ || !impl_) return false;

    try {
        // Create capture item
        if (target.type == CaptureTarget::Type::Window) {
            impl_->item = CreateItemForWindow(static_cast<HWND>(target.handle));
        } else {
            impl_->item = CreateItemForMonitor(static_cast<HMONITOR>(target.handle));
        }

        auto size = impl_->item.Size();
        width_ = static_cast<uint32_t>(size.Width);
        height_ = static_cast<uint32_t>(size.Height);

        // Create frame pool (3 buffers for triple-buffering, free-threaded delivery)
        impl_->frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->winrt_device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            size);

        // Subscribe to frame arrived events
        impl_->frame_arrived_token = impl_->frame_pool.FrameArrived(
            [this](Direct3D11CaptureFramePool const& sender,
                   winrt::Windows::Foundation::IInspectable const&) {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                auto content_size = frame.ContentSize();
                uint32_t w = static_cast<uint32_t>(content_size.Width);
                uint32_t h = static_cast<uint32_t>(content_size.Height);

                // Get D3D11 texture from WinRT surface
                auto surface = frame.Surface();
                auto access = surface.as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

                ComPtr<ID3D11Texture2D> texture;
                access->GetInterface(IID_PPV_ARGS(&texture));

                // Update dimensions if changed
                if (w != width_ || h != height_) {
                    width_ = w;
                    height_ = h;
                    // Recreate pool for new size
                    impl_->frame_pool.Recreate(
                        impl_->winrt_device,
                        DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        3,
                        content_size);
                }

                if (on_frame)
                    on_frame(texture.Get(), w, h);

                frame.Close();
            });

        // Create and start capture session
        impl_->session = impl_->frame_pool.CreateCaptureSession(impl_->item);

        // Remove yellow border (Win10 2004+)
        try {
            impl_->session.IsBorderRequired(false);
        } catch (...) {
            // Older Windows version — border will be shown
        }

        impl_->session.StartCapture();
        capturing_ = true;
        return true;

    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "[ScreenCapture] Failed to start capture: 0x%08x\n",
                     static_cast<unsigned>(e.code()));
        return false;
    }
}

void ScreenCapture::stop() {
    if (!capturing_ || !impl_) return;

    try {
        if (impl_->session) {
            impl_->session.Close();
            impl_->session = nullptr;
        }
        if (impl_->frame_pool) {
            impl_->frame_pool.Close();
            impl_->frame_pool = nullptr;
        }
        impl_->item = nullptr;
    } catch (...) {
        // Ignore errors during cleanup
    }

    capturing_ = false;
    width_ = 0;
    height_ = 0;
}

} // namespace parties::client
