#pragma once

#include <client/rmlui_backend.h>

#include <RmlUi/Core.h>

#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

// Forward declarations — avoid pulling windows.h into header
typedef struct HWND__* HWND;
class ExtendedRenderInterface;
class SystemInterface_Win32;
class TextInputMethodEditor_Win32;

namespace parties::client {

class UiManager {
public:
    UiManager();
    ~UiManager();

    // renderer_id: 0=DX12, 1=DX11, 2=DX12WL, 3=Vulkan
    bool init(HWND hwnd, int renderer_id = 0);
    void shutdown();

    Rml::ElementDocument* load_document(const std::string& path);
    void show_document(Rml::ElementDocument* doc);
    void hide_document(Rml::ElementDocument* doc);
    void unload_all();

    void update();
    void render();
    void render_begin();
    void render_body();
    void render_end();

    void on_resize(int width, int height);
    void on_minimize();
    void on_dpi_change(float scale);

    Rml::Context* context() { return context_; }

    using EventCallback = std::function<void(Rml::Event&)>;
    void bind_event(const std::string& element_id, const std::string& event_type,
                    EventCallback callback);

    // Window controls
    void minimize_window();
    void toggle_maximize();
    void close_window();
    bool is_maximized() const;
    void set_fullscreen(bool fs);
    bool is_fullscreen() const { return fullscreen_; }

    // Accessors for WndProc in main.cpp
    TextInputMethodEditor_Win32& text_input_editor();
    ExtendedRenderInterface* renderer() { return render_interface_.get(); }
    float dpi_scale() const { return dpi_scale_; }
    bool is_minimized() const { return minimized_; }

private:
    std::unique_ptr<ExtendedRenderInterface> render_interface_;
    std::unique_ptr<SystemInterface_Win32> system_interface_;
    std::unique_ptr<TextInputMethodEditor_Win32> text_input_editor_;
    EmbeddedFileInterface file_interface_;

    Rml::Context* context_ = nullptr;
    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;
    bool initialised_ = false;
    bool minimized_ = false;
    bool fullscreen_ = false;
    long saved_style_ = 0;
    long saved_ex_style_ = 0;
    int saved_rect_[4]{};  // left, top, right, bottom before fullscreen
    class EventListenerInstancer;
    class GenericEventListener;
};

} // namespace parties::client
