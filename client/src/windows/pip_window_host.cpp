#include "pip_window_host.h"

#include <client/video_element.h>
#include <parties/log.h>

#include "RmlUi_Platform_Win32.h"
#include "RmlUi_RenderInterface_Extended.h"
#include "dx12/Parties_Renderer_DX12.h"

#include <RmlUi/Core.h>

#include <cmath>
#include <string>

namespace parties::client {

namespace {

constexpr const char* kContextName = "picture-in-picture";

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

} // namespace

PipWindowHost::PipWindowHost() = default;
PipWindowHost::~PipWindowHost() { shutdown(); }

void PipWindowHost::init(std::recursive_mutex* ui_mutex, void* d3d12_device, Actions actions) {
    ui_mutex_ = ui_mutex;
    d3d12_device_ = d3d12_device;
    actions_ = std::move(actions);
}

bool PipWindowHost::ensure_surface() {
    if (context_) return true;
    if (failed_) return false;

    Win32PipWindow::Callbacks callbacks;
    callbacks.is_action_at = [this](int x, int y) {
        std::lock_guard<std::recursive_mutex> lock(*ui_mutex_);
        if (!context_ || !model_ || !model_->overlay_visible.get()) return false;
        return PipWindowModel::is_action(context_->GetElementAtPoint(
            {static_cast<float>(x), static_cast<float>(y)}));
    };
    callbacks.takes_keyboard_at = [this](int x, int y) {
        std::lock_guard<std::recursive_mutex> lock(*ui_mutex_);
        if (!context_ || !model_ || !model_->overlay_visible.get()) return false;
        return PipWindowModel::takes_keyboard(context_->GetElementAtPoint(
            {static_cast<float>(x), static_cast<float>(y)}));
    };
    callbacks.on_input = [this](UINT message, WPARAM w_param, LPARAM l_param) {
        std::lock_guard<std::recursive_mutex> lock(*ui_mutex_);
        if (!context_ || !text_input_) return false;
        return !RmlWin32::WindowProcedure(context_, *text_input_, window_.hwnd(),
                                          message, w_param, l_param);
    };
    callbacks.on_resized = [this](int width, int height, float scale) {
        pending_width_.store(width, std::memory_order_relaxed);
        pending_height_.store(height, std::memory_order_relaxed);
        pending_scale_.store(scale, std::memory_order_relaxed);
        resize_pending_.store(true, std::memory_order_release);
    };
    callbacks.on_geometry_changed = [this](const PipRect& rect) {
        if (actions_.save_geometry) actions_.save_geometry(rect);
    };
    if (!window_.create(std::move(callbacks))) {
        LOG_ERROR("Picture-in-picture window creation failed");
        failed_ = true;
        return false;
    }

    Backend::RmlRendererSettings settings{};
    settings.vsync = false;           // the main window's present paces the render thread
    settings.msaa_sample_count = 1;   // video plus a few overlay controls: no geometry to smooth
    auto renderer = std::make_unique<PartiesRenderInterface_DX12>(window_.hwnd(), settings);
    if (!renderer || !*renderer) {
        LOG_ERROR("Picture-in-picture renderer creation failed");
        failed_ = true;
        return false;
    }
    // D3D12 returns one device per adapter, so this matches unless adapter
    // selection diverged. Decoder surfaces are bound to that device.
    if (d3d12_device_ && renderer->GetD3D12Device() != d3d12_device_) {
        LOG_ERROR("Picture-in-picture renderer is not on the decoder's D3D12 device");
        failed_ = true;
        return false;   // no context used it yet, so it may be destroyed now
    }
    renderer_ = std::move(renderer);

    RECT client{};
    GetClientRect(window_.hwnd(), &client);
    const int width = (std::max)(1, static_cast<int>(client.right));
    const int height = (std::max)(1, static_cast<int>(client.bottom));
    renderer_->SetViewport(width, height, true);
    text_input_ = std::make_unique<TextInputMethodEditor_Win32>();
    context_ = Rml::CreateContext(kContextName, {width, height}, renderer_.get(), text_input_.get());
    if (!context_) {
        LOG_ERROR("Picture-in-picture RmlUi context creation failed");
        text_input_.reset();
        failed_ = true;
        return false;
    }
    VideoElement::RegisterContextRenderInterface(context_, renderer_.get());
    context_->SetDensityIndependentPixelRatio(static_cast<float>(GetDpiForWindow(window_.hwnd())) / 96.0f);

    model_ = std::make_unique<PipWindowModel>();
    model_->on_return = [this] { if (actions_.return_to_main) actions_.return_to_main(); };
    model_->on_volume_changed = [this](float volume) { if (actions_.set_volume) actions_.set_volume(volume); };
    model_->on_close = [this] { if (actions_.close) actions_.close(); };
    if (!model_->init(context_) || !(document_ = context_->LoadDocument("ui/pip.rml"))) {
        LOG_ERROR("Picture-in-picture document failed to load");
        failed_ = true;
        prepare_shutdown();
        return false;
    }
    document_->SetClass("platform-windows", true);
    document_->SetClass("platform-desktop", true);
    document_->Show();
    video_ = rmlui_dynamic_cast<VideoElement*>(document_->GetElementById("pip-video"));
    return true;
}

bool PipWindowHost::show(const std::string& title) {
    if (!ensure_surface()) return false;
    model_->title = title;
    SetWindowTextW(window_.hwnd(), widen(title.empty() ? "Parties picture-in-picture" : title).c_str());
    if (window_.visible()) return true;   // switching streams keeps the window where it is

    const float scale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    const auto remembered = actions_.load_geometry ? actions_.load_geometry() : std::nullopt;
    window_.show(Win32PipWindow::placement(remembered, window_.aspect(),
                                           Win32PipWindow::visible_work_areas(), scale));
    return true;
}

void PipWindowHost::hide() {
    if (!window_.visible()) return;
    if (actions_.save_geometry) actions_.save_geometry(window_.window_rect());
    window_.hide();
    if (model_) model_->overlay_visible = false;
}

void PipWindowHost::apply_pending_resize() {
    if (!resize_pending_.exchange(false, std::memory_order_acquire)) return;
    const int width = pending_width_.load(std::memory_order_relaxed);
    const int height = pending_height_.load(std::memory_order_relaxed);
    if (width <= 0 || height <= 0) return;
    renderer_->SetViewport(width, height, true);
    context_->SetDimensions({width, height});
    context_->SetDensityIndependentPixelRatio(pending_scale_.load(std::memory_order_relaxed));
}

void PipWindowHost::fit_window_to_video() {
    if (!video_ || video_->frame_width() == 0 || video_->frame_height() == 0) return;
    const double aspect = static_cast<double>(video_->frame_width()) / video_->frame_height();
    if (std::fabs(aspect - fitted_aspect_) < 0.005) return;
    fitted_aspect_ = aspect;
    window_.set_aspect(aspect);
    const PipRect rect = window_.window_rect();
    if (rect.width <= 0.0) return;
    // Keep the width the user chose; the owner thread applies it and keeps
    // the window on its monitor.
    window_.request_size(static_cast<int>(std::lround(rect.width)),
                         static_cast<int>(std::lround(pip_height_for_width(rect.width, aspect))));
}

void PipWindowHost::render(float volume) {
    if (!context_ || !renderer_ || !window_.visible()) return;
    apply_pending_resize();
    // Stay visible while a control is in use, e.g. a volume drag that left
    // the window or keyboard adjustment after the cursor moved away.
    model_->overlay_visible = window_.cursor_inside() || window_.interacting();
    model_->volume = volume;
    fit_window_to_video();

    context_->Update();
    renderer_->BeginFrame();
    if (!renderer_->IsFrameActive()) return;
    renderer_->Clear();
    context_->Render();
    renderer_->EndFrame();
}

void PipWindowHost::prepare_shutdown() {
    window_.hide();
    if (context_) {
        context_->UnloadAllDocuments();
        Rml::RemoveContext(kContextName);
        VideoElement::UnregisterContextRenderInterface(context_);
        context_ = nullptr;
    }
    document_ = nullptr;
    video_ = nullptr;
    model_.reset();
    text_input_.reset();
}

void PipWindowHost::shutdown() {
    // Rml::Shutdown has completed: no render manager refers to the renderer.
    renderer_.reset();
    window_.destroy();
}

} // namespace parties::client
