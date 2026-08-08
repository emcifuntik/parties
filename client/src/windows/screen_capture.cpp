#include <client/screen_capture.h>
#include <parties/profiler.h>
#include <parties/log.h>

// WinRT / Windows Graphics Capture headers
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dwmapi.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace parties::client {

namespace {

// Windows.Graphics.Capture activation and one-shot session creation cross an
// out-of-process COM/RPC boundary. Concurrent callers can wedge the capture
// broker and leave every caller waiting in NtAlpcSendWaitReceivePort. Keep this
// critical system interaction serialized; frame delivery remains concurrent.
std::mutex g_wgc_session_setup_mutex;

} // namespace

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

// ─── One-shot WGC thumbnails ─────────────────────────────────────────────────

namespace {

// The picker captures every application from one background thread. Reusing a
// D3D11 device avoids recreating an adapter/device for every card while keeping
// the live stream capture session completely independent.
struct ThumbnailCaptureContext {
    HRESULT apartment_result = E_FAIL;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    IDirect3DDevice winrt_device{nullptr};

    ThumbnailCaptureContext() {
        apartment_result = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(apartment_result) && apartment_result != RPC_E_CHANGED_MODE)
            return;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL feature_level{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &device, &feature_level, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION,
                &device, &feature_level, &context);
        }
        if (FAILED(hr)) {
            device.Reset();
            context.Reset();
            return;
        }

        ComPtr<ID3D10Multithread> multithread;
        device.As(&multithread);
        if (multithread)
            multithread->SetMultithreadProtected(TRUE);

        try {
            winrt_device = CreateWinRTDevice(device.Get());
        } catch (...) {
            winrt_device = nullptr;
            context.Reset();
            device.Reset();
        }
    }

    ~ThumbnailCaptureContext() {
        winrt_device = nullptr;
        context.Reset();
        device.Reset();
        if (SUCCEEDED(apartment_result))
            RoUninitialize();
    }

    explicit operator bool() const {
        return device && context && winrt_device;
    }
};

static CaptureThumbnail ReadThumbnailTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    uint32_t content_width,
    uint32_t content_height,
    uint32_t max_width,
    uint32_t max_height) {
    CaptureThumbnail result;
    if (!device || !context || !texture || content_width == 0 || content_height == 0)
        return result;

    D3D11_TEXTURE2D_DESC source_desc{};
    texture->GetDesc(&source_desc);
    const uint32_t source_width = (std::min)(content_width, source_desc.Width);
    const uint32_t source_height = (std::min)(content_height, source_desc.Height);
    if (source_width == 0 || source_height == 0)
        return result;

    const double scale_x = static_cast<double>(max_width) / source_width;
    const double scale_y = static_cast<double>(max_height) / source_height;
    const double scale = (std::min)(1.0, (std::min)(scale_x, scale_y));
    const uint32_t thumb_width = (std::max)(1u, static_cast<uint32_t>(source_width * scale));
    const uint32_t thumb_height = (std::max)(1u, static_cast<uint32_t>(source_height * scale));

    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.Width = source_width;
    staging_desc.Height = source_height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging)))
        return result;

    D3D11_BOX source_box{0, 0, 0, source_width, source_height, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &source_box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return result;

    result.width = thumb_width;
    result.height = thumb_height;
    result.source = CaptureThumbnail::Source::WindowsGraphicsCapture;
    result.rgba.resize(static_cast<size_t>(thumb_width) * thumb_height * 4);

    // Bilinear sampling happens only for the 320x180 picker texture. It avoids
    // uploading a full-sized window to RmlUi and keeps text/icon previews crisp.
    for (uint32_t y = 0; y < thumb_height; ++y) {
        const double source_y = ((static_cast<double>(y) + 0.5) * source_height /
                                 thumb_height) - 0.5;
        const uint32_t y0 = static_cast<uint32_t>((std::max)(0.0, std::floor(source_y)));
        const uint32_t y1 = (std::min)(source_height - 1, y0 + 1);
        const double fy = (std::max)(0.0, source_y - y0);
        const auto* row0 = static_cast<const uint8_t*>(mapped.pData) +
                           static_cast<size_t>(y0) * mapped.RowPitch;
        const auto* row1 = static_cast<const uint8_t*>(mapped.pData) +
                           static_cast<size_t>(y1) * mapped.RowPitch;

        for (uint32_t x = 0; x < thumb_width; ++x) {
            const double source_x = ((static_cast<double>(x) + 0.5) * source_width /
                                     thumb_width) - 0.5;
            const uint32_t x0 = static_cast<uint32_t>((std::max)(0.0, std::floor(source_x)));
            const uint32_t x1 = (std::min)(source_width - 1, x0 + 1);
            const double fx = (std::max)(0.0, source_x - x0);
            const auto* p00 = row0 + static_cast<size_t>(x0) * 4;
            const auto* p10 = row0 + static_cast<size_t>(x1) * 4;
            const auto* p01 = row1 + static_cast<size_t>(x0) * 4;
            const auto* p11 = row1 + static_cast<size_t>(x1) * 4;
            auto* destination = result.rgba.data() +
                                (static_cast<size_t>(y) * thumb_width + x) * 4;

            for (int rgba_channel = 0; rgba_channel < 3; ++rgba_channel) {
                // WGC is BGRA, while VideoElement::UpdateFrame accepts RGBA.
                const int bgra_channel = 2 - rgba_channel;
                const double top = p00[bgra_channel] +
                                   (p10[bgra_channel] - p00[bgra_channel]) * fx;
                const double bottom = p01[bgra_channel] +
                                      (p11[bgra_channel] - p01[bgra_channel]) * fx;
                destination[rgba_channel] = static_cast<uint8_t>(
                    (std::clamp)(std::lround(top + (bottom - top) * fy), 0l, 255l));
            }
            destination[3] = 255;
        }
    }

    context->Unmap(staging.Get(), 0);
    return result;
}

struct OneShotThumbnailState {
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic_flag frame_claimed = ATOMIC_FLAG_INIT;
    bool complete = false;
    CaptureThumbnail thumbnail;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
};

static CaptureThumbnail CaptureWindowThumbnailWgc(ThumbnailCaptureContext& capture_context,
                                                   HWND window,
                                                   uint32_t max_width,
                                                   uint32_t max_height) {
    if (!window || !IsWindow(window))
        return {};

    if (!capture_context)
        return {};

    std::lock_guard setup_lock(g_wgc_session_setup_mutex);
    try {
        if (!GraphicsCaptureSession::IsSupported())
            return {};

        auto item = CreateItemForWindow(window);
        const auto item_size = item.Size();
        if (item_size.Width <= 0 || item_size.Height <= 0)
            return {};

        auto state = std::make_shared<OneShotThumbnailState>();
        state->device = capture_context.device;
        state->context = capture_context.context;
        state->max_width = max_width;
        state->max_height = max_height;

        auto frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            capture_context.winrt_device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,
            item_size);

        const event_token frame_token = frame_pool.FrameArrived(
            [state](Direct3D11CaptureFramePool const& sender,
                    winrt::Windows::Foundation::IInspectable const&) {
                if (state->frame_claimed.test_and_set(std::memory_order_acq_rel))
                    return;

                CaptureThumbnail thumbnail;
                try {
                    auto frame = sender.TryGetNextFrame();
                    if (frame) {
                        const auto size = frame.ContentSize();
                        auto access = frame.Surface().as<
                            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        ComPtr<ID3D11Texture2D> texture;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&texture))) && texture) {
                            thumbnail = ReadThumbnailTexture(
                                state->device.Get(), state->context.Get(), texture.Get(),
                                static_cast<uint32_t>((std::max)(0, size.Width)),
                                static_cast<uint32_t>((std::max)(0, size.Height)),
                                state->max_width, state->max_height);
                        }
                        frame.Close();
                    }
                } catch (...) {
                    // The target may disappear between enumeration and capture.
                }

                {
                    std::lock_guard lock(state->mutex);
                    state->thumbnail = std::move(thumbnail);
                    state->complete = true;
                }
                state->ready.notify_one();
            });

        auto session = frame_pool.CreateCaptureSession(item);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try { session.IsBorderRequired(false); } catch (...) {}
        }
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
            try { session.IsCursorCaptureEnabled(false); } catch (...) {}
        }
        session.StartCapture();

        CaptureThumbnail result;
        {
            std::unique_lock lock(state->mutex);
            // A composited frame normally arrives in one refresh interval. The
            // bounded wait prevents a protected/hung target from stalling every
            // card that follows it in the picker.
            state->ready.wait_for(lock, std::chrono::milliseconds(350),
                                  [&state] { return state->complete; });
            if (state->complete)
                result = std::move(state->thumbnail);
        }

        try { frame_pool.FrameArrived(frame_token); } catch (...) {}
        try { session.Close(); } catch (...) {}
        try { frame_pool.Close(); } catch (...) {}
        return result;
    } catch (const winrt::hresult_error& error) {
        LOG_WARN("WGC thumbnail capture failed: {:#010x}",
                 static_cast<unsigned>(error.code()));
        return {};
    } catch (...) {
        return {};
    }
}

} // namespace

struct ScreenCapture::ThumbnailSession::Impl {
    ThumbnailCaptureContext capture_context;
};

ScreenCapture::ThumbnailSession::ThumbnailSession()
    : impl_(std::make_unique<Impl>()) {}

ScreenCapture::ThumbnailSession::~ThumbnailSession() = default;

// ─── Pimpl holding WinRT capture state ───────────────────────────────────────

struct ScreenCapture::Impl {
    IDirect3DDevice winrt_device{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool frame_pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    event_token frame_arrived_token{};
    event_token closed_token{};
};

// ─── ScreenCapture implementation ────────────────────────────────────────────

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    shutdown();
}

bool ScreenCapture::init() {
	ZoneScopedN("ScreenCapture::init");

    // Enumerate adapters to find the one driving the displays.
    // Using the wrong adapter forces WGC to do cross-adapter copies (~50fps cap).
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> best_adapter;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        // Check if this adapter has any active outputs (monitors connected)
        ComPtr<IDXGIOutput> output;
        bool has_outputs = (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND);

        if (has_outputs && !best_adapter)
            best_adapter = adapter;

        adapter.Reset();
    }

    // Create D3D11 device on the adapter with outputs (or default if none found)
    D3D_FEATURE_LEVEL feature_level;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr;

    if (best_adapter) {
        hr = D3D11CreateDevice(
            best_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &feature_level, &context_);
        if (FAILED(hr)) {
            flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            hr = D3D11CreateDevice(
                best_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION,
                &device_, &feature_level, &context_);
        }
    } else {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &feature_level, &context_);
        if (FAILED(hr)) {
            flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION,
                &device_, &feature_level, &context_);
        }
    }

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 device ({:#010x})", static_cast<unsigned>(hr));
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
        LOG_ERROR("WinRT device creation failed: {:#010x}",
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
	ZoneScopedN("ScreenCapture::enumerate_windows");
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
	ZoneScopedN("ScreenCapture::enumerate_monitors");
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

CaptureThumbnail ScreenCapture::ThumbnailSession::capture(const CaptureTarget& target,
                                                          uint32_t max_width,
                                                          uint32_t max_height) {
    ZoneScopedN("ScreenCapture::ThumbnailSession::capture");
    CaptureThumbnail result;
    if (!target.handle || max_width == 0 || max_height == 0)
        return result;

    if (target.type == CaptureTarget::Type::Window && impl_) {
        result = CaptureWindowThumbnailWgc(
            impl_->capture_context, static_cast<HWND>(target.handle),
            max_width, max_height);
        if (result)
            return result;
    }

    HDC source_dc = nullptr;
    HWND source_window = nullptr;
    int source_x = 0;
    int source_y = 0;
    int source_width = 0;
    int source_height = 0;

    if (target.type == CaptureTarget::Type::Monitor) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(static_cast<HMONITOR>(target.handle), &info))
            return result;
        source_x = info.rcMonitor.left;
        source_y = info.rcMonitor.top;
        source_width = info.rcMonitor.right - info.rcMonitor.left;
        source_height = info.rcMonitor.bottom - info.rcMonitor.top;
        source_dc = GetDC(nullptr);
    } else {
        source_window = static_cast<HWND>(target.handle);
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(source_window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &bounds, sizeof(bounds))) &&
            !GetWindowRect(source_window, &bounds))
            return result;
        source_width = bounds.right - bounds.left;
        source_height = bounds.bottom - bounds.top;
        // A window DC captures the actual target surface without copying the
        // desktop around it. Some protected surfaces may still decline capture;
        // the picker retains a clear fallback for those targets.
        source_dc = GetWindowDC(source_window);
    }

    if (!source_dc || source_width <= 0 || source_height <= 0) {
        if (source_dc)
            ReleaseDC(source_window, source_dc);
        return result;
    }

    const double scale_x = static_cast<double>(max_width) / source_width;
    const double scale_y = static_cast<double>(max_height) / source_height;
    const double scale = (std::min)(1.0, (std::min)(scale_x, scale_y));
    const int thumb_width = (std::max)(1, static_cast<int>(source_width * scale));
    const int thumb_height = (std::max)(1, static_cast<int>(source_height * scale));

    HDC memory_dc = CreateCompatibleDC(source_dc);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = thumb_width;
    bitmap_info.bmiHeader.biHeight = -thumb_height; // top-down pixels
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* dib_pixels = nullptr;
    HBITMAP bitmap = memory_dc
        ? CreateDIBSection(memory_dc, &bitmap_info, DIB_RGB_COLORS, &dib_pixels, nullptr, 0)
        : nullptr;
    HGDIOBJ previous = bitmap ? SelectObject(memory_dc, bitmap) : nullptr;
    bool captured = false;
    if (bitmap && dib_pixels) {
        SetStretchBltMode(memory_dc, HALFTONE);
        SetBrushOrgEx(memory_dc, 0, 0, nullptr);
        captured = StretchBlt(memory_dc, 0, 0, thumb_width, thumb_height,
                              source_dc, source_x, source_y, source_width, source_height,
                              SRCCOPY | CAPTUREBLT) != FALSE;
    }

    if (captured) {
        result.width = static_cast<uint32_t>(thumb_width);
        result.height = static_cast<uint32_t>(thumb_height);
        result.source = CaptureThumbnail::Source::Gdi;
        result.rgba.resize(static_cast<size_t>(thumb_width) * thumb_height * 4);
        const auto* bgra = static_cast<const uint8_t*>(dib_pixels);
        for (size_t pixel = 0; pixel < result.rgba.size(); pixel += 4) {
            result.rgba[pixel + 0] = bgra[pixel + 2];
            result.rgba[pixel + 1] = bgra[pixel + 1];
            result.rgba[pixel + 2] = bgra[pixel + 0];
            result.rgba[pixel + 3] = 255;
        }
    }

    if (previous)
        SelectObject(memory_dc, previous);
    if (bitmap)
        DeleteObject(bitmap);
    if (memory_dc)
        DeleteDC(memory_dc);
    ReleaseDC(source_window, source_dc);
    return result;
}

bool ScreenCapture::start(const CaptureTarget& target, uint32_t target_fps) {
	ZoneScopedN("ScreenCapture::start");
    if (capturing_ || !impl_) return false;

    std::lock_guard setup_lock(g_wgc_session_setup_mutex);
    frame_count_ = 0;

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
        const auto max_frame_time = std::chrono::milliseconds(1000 / target_fps);

        // Subscribe to item closed (window closed / monitor disconnected)
        impl_->closed_token = impl_->item.Closed(
            [this](GraphicsCaptureItem const&, winrt::Windows::Foundation::IInspectable const&) {
                LOG_WARN("Capture item closed");
                capturing_ = false;
                if (on_closed) on_closed();
            });

        // 3 buffers: compositor holds 1, callback holds 1, 1 spare.
        impl_->frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->winrt_device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            size);

        // Subscribe to frame arrived events
        impl_->frame_arrived_token = impl_->frame_pool.FrameArrived(
            [this, max_frame_time](Direct3D11CaptureFramePool const& sender,
                   winrt::Windows::Foundation::IInspectable const&) {
                ZoneScopedN("ScreenCapture::FrameArrived");
                thread_local bool named = (TracySetThreadName("ScreenCapture"), true);
                (void)named;

                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                // Software frame rate limiting for older Windows without MinUpdateInterval
                if (!frame_limited_) {
                    auto now = std::chrono::steady_clock::now();
                    auto limit = max_frame_time * 9 / 10;  // 90% of target interval
                    if (now - last_frame_time_ < limit) {
                        frame.Close();
                        return;
                    }
                    last_frame_time_ = now;
                }

                auto content_size = frame.ContentSize();
                uint32_t w = static_cast<uint32_t>(content_size.Width);
                uint32_t h = static_cast<uint32_t>(content_size.Height);

                // Get D3D11 texture from WinRT surface
                auto surface = frame.Surface();
                auto access = surface.as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

                ComPtr<ID3D11Texture2D> texture;
                HRESULT hr = access->GetInterface(IID_PPV_ARGS(&texture));
                if (FAILED(hr) || !texture) { frame.Close(); return; }

                frame_count_++;

                // Update dimensions if changed — recreate pool and skip this frame
                if (w != width_ || h != height_) {
                    LOG_WARN("size changed: {}x{} -> {}x{}",
                                 width_, height_, w, h);
                    width_ = w;
                    height_ = h;
                    frame.Close();
                    impl_->frame_pool.Recreate(
                        impl_->winrt_device,
                        DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        3,
                        content_size);
                    return;
                }

                if (on_frame)
                    on_frame(texture.Get(), w, h);

                // Close AFTER on_frame so the WGC pool texture stays valid
                // during CopyResource (prevents D3D11 hazard shadow copies).
                frame.Close();
            });

        // Create and start capture session
        impl_->session = impl_->frame_pool.CreateCaptureSession(impl_->item);

        // Remove yellow border and disable cursor compositing (Win10 2004+)
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
        {
            try {
                impl_->session.IsBorderRequired(false);
            } catch (...) {}
        }

        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
        {
            try {
                impl_->session.IsCursorCaptureEnabled(true);
            } catch (...) {}
        }

        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval"))
        {
            try {
                impl_->session.MinUpdateInterval(max_frame_time);
                frame_limited_ = true;
            }
            catch (...) {}
        }

        impl_->session.StartCapture();
        capturing_ = true;
        return true;

    } catch (const winrt::hresult_error& e) {
        LOG_ERROR("Failed to start capture: {:#010x}",
                     static_cast<unsigned>(e.code()));
        return false;
    }
}

void ScreenCapture::stop() {
	ZoneScopedN("ScreenCapture::stop");
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
