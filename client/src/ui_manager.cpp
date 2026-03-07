#include <client/ui_manager.h>

#include "RmlUi_Renderer_DX12.h"
#include "RmlUi_Platform_Win32.h"

#include <RmlUi/Debugger.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <cstdio>
#include <cstring>

namespace parties::client {

// ═══════════════════════════════════════════════════════════════════════
// Event listener helper
// ═══════════════════════════════════════════════════════════════════════

class UiManager::GenericEventListener : public Rml::EventListener {
public:
    GenericEventListener(EventCallback cb) : callback_(std::move(cb)) {}
    void ProcessEvent(Rml::Event& event) override { callback_(event); }

private:
    EventCallback callback_;
};

// ═══════════════════════════════════════════════════════════════════════
// UiManager
// ═══════════════════════════════════════════════════════════════════════

UiManager::UiManager() = default;
UiManager::~UiManager() { shutdown(); }

bool UiManager::init(HWND hwnd) {
    hwnd_ = hwnd;

    // Create system interface (from vendored Win32 platform)
    system_interface_ = std::make_unique<SystemInterface_Win32>();
    system_interface_->SetWindow(hwnd);

    Rml::SetSystemInterface(system_interface_.get());
    Rml::SetFileInterface(&file_interface_);

    // Create DX12 render interface
    Backend::RmlRendererSettings settings{};
    settings.vsync = true;
    settings.msaa_sample_count = 1;

    render_interface_ = std::make_unique<RenderInterface_DX12>(
        static_cast<void*>(hwnd), settings);
    if (!*render_interface_) {
        std::fprintf(stderr, "[UI] Failed to create DX12 render interface\n");
        return false;
    }

    Rml::SetRenderInterface(render_interface_.get());

    if (!Rml::Initialise()) {
        std::fprintf(stderr, "[UI] Failed to initialise RmlUi\n");
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
        std::fprintf(stderr, "[UI] Failed to create RmlUi context\n");
        Rml::Shutdown();
        return false;
    }

    Rml::Debugger::Initialise(context_);

    // DPI scaling
    UINT dpi = GetDpiForWindow(hwnd);
    dpi_scale_ = static_cast<float>(dpi) / 96.0f;
    context_->SetDensityIndependentPixelRatio(dpi_scale_);

    // Load fonts
    if (!Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf"))
        Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

    // Text input method editor for IME support
    text_input_editor_ = std::make_unique<TextInputMethodEditor_Win32>();

    initialised_ = true;
    std::printf("[UI] Initialised DX12 (%dx%d, DPI scale=%.2f)\n", width, height, dpi_scale_);
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
        std::fprintf(stderr, "[UI] Failed to load document: %s\n", path.c_str());
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
    if (context_) context_->Update();
}

void UiManager::render() {
    render_begin();
    render_body();
    render_end();
}

void UiManager::render_begin() {
    if (!render_interface_ || !*render_interface_ || minimized_) return;
    render_interface_->BeginFrame();
}

void UiManager::render_body() {
    if (!render_interface_ || !*render_interface_ || minimized_) return;
    render_interface_->Clear();
    if (context_) context_->Render();
}

void UiManager::render_end() {
    if (!render_interface_ || !*render_interface_ || minimized_) return;
    render_interface_->EndFrame();
}

void UiManager::on_resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    bool was_minimized = minimized_;
    minimized_ = false;
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
    minimized_ = true;
}

void UiManager::on_dpi_change(float scale) {
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
            auto* listener = new GenericEventListener(std::move(callback));
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

TextInputMethodEditor_Win32& UiManager::text_input_editor() {
    return *text_input_editor_;
}

} // namespace parties::client
