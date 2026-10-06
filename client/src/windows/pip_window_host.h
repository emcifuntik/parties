#pragma once

#include "win32_pip_window.h"

#include <client/pip_window_model.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class ExtendedRenderInterface;
class TextInputMethodEditor_Win32;
namespace Rml { class Context; class ElementDocument; }

namespace parties::client {

class VideoElement;

// Windows picture-in-picture presenter: the Win32PipWindow shell plus its own
// DX12 renderer, RmlUi context, ui/pip.rml document and PipWindowModel.
//
// The renderer must use the decoder's D3D12 device so native (AMF/NVDEC)
// frames can be sampled; open fails otherwise. Like ContextWindowManager, the
// HWND, renderer and context are created on first use and kept until
// shutdown: RmlUi retains a render manager per render interface until
// Rml::Shutdown, so the renderer cannot be destroyed earlier.
//
// Threads: show/hide and the window procedure run on the message thread with
// the UI mutex held; video_surface/render on the render thread with the UI
// mutex held.
class PipWindowHost {
public:
    struct Actions {
        std::function<void()> return_to_main;
        std::function<void(float)> set_volume;   // stream playback volume
        std::function<void()> close;
        std::function<std::optional<PipRect>()> load_geometry;
        std::function<void(const PipRect&)> save_geometry;
    };

    PipWindowHost();
    ~PipWindowHost();
    PipWindowHost(const PipWindowHost&) = delete;
    PipWindowHost& operator=(const PipWindowHost&) = delete;

    void init(std::recursive_mutex* ui_mutex, void* d3d12_device, Actions actions);

    // Message thread. False if the window/renderer could not be created.
    bool show(const std::string& title);
    void hide();
    bool visible() const { return window_.visible(); }

    // Render thread. The PiP video surface (null before the first show).
    VideoElement* video_surface() const { return video_; }
    ExtendedRenderInterface* renderer() const { return renderer_.get(); }
    // Render one PiP frame if the window is visible, mirroring the current
    // stream playback volume into the overlay.
    void render(float volume);

    // Before Rml::Shutdown: unload the document and remove the context.
    void prepare_shutdown();
    // After Rml::Shutdown: release the renderer and destroy the HWND.
    void shutdown();

private:
    bool ensure_surface();
    void apply_pending_resize();
    void fit_window_to_video();

    std::recursive_mutex* ui_mutex_ = nullptr;
    void* d3d12_device_ = nullptr;
    Actions actions_;

    Win32PipWindow window_;
    std::unique_ptr<ExtendedRenderInterface> renderer_;
    std::unique_ptr<TextInputMethodEditor_Win32> text_input_;
    std::unique_ptr<PipWindowModel> model_;
    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    VideoElement* video_ = nullptr;
    bool failed_ = false;

    // Written by the window procedure, applied by the render thread.
    std::atomic<bool> resize_pending_{false};
    std::atomic<int> pending_width_{0};
    std::atomic<int> pending_height_{0};
    std::atomic<float> pending_scale_{1.0f};
    double fitted_aspect_ = 0.0;   // render thread
};

} // namespace parties::client
