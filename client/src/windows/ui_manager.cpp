#include <client/ui_manager.h>
#include <client/rml_elements.h>
#include <parties/profiler.h>

#include "RmlUi_RenderInterface_Extended.h"
#include "dx12/Parties_Renderer_DX12.h"
#include "RmlUi_Platform_Win32.h"

#ifndef PARTIES_RETAIL
#include <RmlUi/Debugger.h>
#endif
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <parties/log.h>

#include <cstring>

namespace parties::client {

namespace {

class PartiesSystemInterface_Win32 final : public SystemInterface_Win32 {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            LOG_ERROR("RmlUi: {}", message);
            break;
        case Rml::Log::LT_WARNING:
            LOG_WARN("RmlUi: {}", message);
            break;
        case Rml::Log::LT_DEBUG:
            LOG_DEBUG("RmlUi: {}", message);
            break;
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_INFO:
        default:
            LOG_INFO("RmlUi: {}", message);
            break;
        }
        // Production diagnostics must never open a debugger assertion dialog.
        return true;
    }
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// UiManager
// ═══════════════════════════════════════════════════════════════════════

UiManager::UiManager() = default;
UiManager::~UiManager() { shutdown(); }

bool UiManager::init(HWND hwnd) {
	ZoneScopedN("UiManager::init");
    hwnd_ = hwnd;

    // Create system interface (from vendored Win32 platform)
    system_interface_ = std::make_unique<PartiesSystemInterface_Win32>();
    system_interface_->SetWindow(hwnd);

    Rml::SetSystemInterface(system_interface_.get());
    Rml::SetFileInterface(&file_interface_);

    // Create render interface
    Backend::RmlRendererSettings settings{};
    settings.vsync = true;
    settings.msaa_sample_count = 4;

    render_interface_ = std::make_unique<PartiesRenderInterface_DX12>(
        static_cast<void*>(hwnd), settings);
    if (!*render_interface_) {
        LOG_ERROR("Failed to create the upstream RmlUi Win32/DX12 renderer");
        return false;
    }

    Rml::SetRenderInterface(render_interface_.get());

    if (!Rml::Initialise()) {
        LOG_ERROR("Failed to initialise RmlUi");
        return false;
    }

    // Register custom property for Win32 window hit-testing.
    // Inherited so child elements of .titlebar get 'caption' automatically.
    Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
        .AddParser("keyword", "none, caption, close, minimize, maximize");

    // Get window dimensions
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    render_interface_->SetViewport(width, height);

    // Create context
    context_ = Rml::CreateContext("main", Rml::Vector2i(width, height));
    if (!context_) {
        LOG_ERROR("Failed to create RmlUi context");
        Rml::Shutdown();
        return false;
    }

#ifndef PARTIES_RETAIL
    Rml::Debugger::Initialise(context_);
#endif

    // DPI scaling
    UINT dpi = GetDpiForWindow(hwnd);
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    context_->SetDensityIndependentPixelRatio(dpi_scale_);

    // Load fonts (Inter — Regular, Medium, Bold)
    Rml::LoadFontFace("ui/fonts/Inter-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Medium.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Bold.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf", true);
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

    // Text input method editor for IME support
    text_input_editor_ = std::make_unique<TextInputMethodEditor_Win32>();

    initialised_ = true;
    LOG_INFO("Initialised upstream RmlUi Win32/DX12 backend ({}x{}, DPI scale={:.2f})",
        width, height, dpi_scale_);
    return true;
}

void UiManager::shutdown() {
    if (!initialised_) return;

    text_input_editor_.reset();
    if (context_) {
        Rml::RemoveContext("main");
        context_ = nullptr;
    }
    Rml::Shutdown();

    render_interface_.reset();
    system_interface_.reset();
    initialised_ = false;
}

Rml::ElementDocument* UiManager::load_document(const std::string& path) {
    if (!context_) return nullptr;
    auto* doc = context_->LoadDocument(path);
    if (!doc)
        LOG_ERROR("Failed to load document: {}", path);
    return doc;
}

void UiManager::show_document(Rml::ElementDocument* doc) {
    if (doc) doc->Show();
}

void UiManager::hide_document(Rml::ElementDocument* doc) {
    if (doc) doc->Hide();
}

void UiManager::unload_all() {
    if (context_) context_->UnloadAllDocuments();
}

void UiManager::update() {
	ZoneScopedN("UiManager::update");
    if (context_) context_->Update();
}

void UiManager::render() {
    if (!render_begin()) return;
    render_body();
    render_end();
}

bool UiManager::render_begin() {
	ZoneScopedN("UiManager::render_begin");
    frame_started_ = false;
    if (!render_interface_ || !*render_interface_ ||
        minimized_.load(std::memory_order_acquire)) return false;
    render_interface_->BeginFrame();
    frame_started_ = render_interface_->IsFrameActive();
    return frame_started_;
}

void UiManager::render_body() {
	ZoneScopedN("UiManager::render_body");
    // Do not re-check minimized_ here. A minimize can race with rendering, and
    // abandoning a frame after BeginFrame leaves the DX12 command list open.
    if (!frame_started_ || !render_interface_) return;
    render_interface_->Clear();
    if (context_) context_->Render();
}

void UiManager::render_end() {
	ZoneScopedN("UiManager::render_end");
    // Every successfully opened frame must be closed, including a frame during
    // which the message thread reported that the window was minimized.
    if (!frame_started_ || !render_interface_) return;
    frame_started_ = false;
    render_interface_->EndFrame();
}

void UiManager::on_resize(int width, int height) {
	ZoneScopedN("UiManager::on_resize");
    if (width <= 0 || height <= 0) return;
    bool was_minimized = minimized_.exchange(false, std::memory_order_acq_rel);
    if (render_interface_) {
        // After restore from minimize, force a viewport reset even if
        // dimensions match. ResizeBuffers re-registers the swap chain
        // with the DWM compositor after a minimize/restore cycle.
        render_interface_->SetViewport(width, height, was_minimized);
    }
    if (context_)
        context_->SetDimensions(Rml::Vector2i(width, height));
}

void UiManager::on_minimize() {
    minimized_.store(true, std::memory_order_release);
}

void UiManager::on_dpi_change(float scale) {
	ZoneScopedN("UiManager::on_dpi_change");
    dpi_scale_ = scale;
    if (context_)
        context_->SetDensityIndependentPixelRatio(scale);
}

void UiManager::bind_event(const std::string& element_id, const std::string& event_type,
                            EventCallback callback) {
    if (!context_) return;

    for (int i = 0; i < context_->GetNumDocuments(); i++) {
        auto* doc = context_->GetDocument(i);
        auto* element = doc->GetElementById(element_id);
        if (element) {
            // OwnedListener deletes itself on detach (RmlUi never frees listeners).
            auto* listener = new parties::rml::OwnedListener(std::move(callback));
            element->AddEventListener(event_type, listener);
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Window controls
// ═══════════════════════════════════════════════════════════════════════

void UiManager::minimize_window() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void UiManager::toggle_maximize() {
    if (!hwnd_) return;
    if (IsZoomed(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    else
        ShowWindow(hwnd_, SW_MAXIMIZE);
}

void UiManager::close_window() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool UiManager::is_maximized() const {
    return hwnd_ && IsZoomed(hwnd_);
}

void UiManager::set_fullscreen(bool fs) {
    if (!hwnd_ || fullscreen_ == fs) return;

    if (fs) {
        // Save current window state
        saved_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        saved_ex_style_ = GetWindowLongW(hwnd_, GWL_EXSTYLE);
        RECT rc;
        GetWindowRect(hwnd_, &rc);
        saved_rect_[0] = rc.left;
        saved_rect_[1] = rc.top;
        saved_rect_[2] = rc.right;
        saved_rect_[3] = rc.bottom;

        // Remove caption and thick frame for true borderless fullscreen
        SetWindowLongW(hwnd_, GWL_STYLE,
            saved_style_ & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLongW(hwnd_, GWL_EXSTYLE,
            saved_ex_style_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        // Cover the full monitor (including taskbar)
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        SetWindowPos(hwnd_, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        // Restore saved window state
        SetWindowLongW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, saved_ex_style_);
        SetWindowPos(hwnd_, nullptr,
            saved_rect_[0], saved_rect_[1],
            saved_rect_[2] - saved_rect_[0],
            saved_rect_[3] - saved_rect_[1],
            SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    fullscreen_ = fs;
}

TextInputMethodEditor_Win32& UiManager::text_input_editor() {
    return *text_input_editor_;
}

} // namespace parties::client
