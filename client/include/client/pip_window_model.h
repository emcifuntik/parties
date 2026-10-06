#pragma once

#include <client/rml_binding.h>

#include <functional>

namespace Rml { class Element; }

namespace parties::client {

// Data model of the desktop picture-in-picture document (ui/pip.rml), shared
// by the Windows and macOS PiP hosts. The host owns the window; this model only
// carries what the overlay shows and the overlay actions.
class PipWindowModel final : public rml::Model {
public:
    // Same range as the main stream volume slider (LobbyModel::stream_volume).
    static constexpr float kMaxVolume = 2.0f;

    rml::Prop<Rml::String> title;
    // Stream playback volume, mirrored from the application every frame.
    rml::Prop<float> volume{1.0f};
    // Set by the host while the cursor is over the window or the user is
    // interacting with an overlay control. Nothing is shown otherwise.
    rml::Prop<bool> overlay_visible{false};

    std::function<void()> on_return;                // back to the main window
    std::function<void(float)> on_volume_changed;   // user moved the PiP volume control
    std::function<void()> on_close;                 // close PiP, keep watching in the grid

    // Overlay elements (class "pip-action") receive clicks; everything else
    // drags the window.
    static bool is_action(Rml::Element* element);
    // Overlay controls (class "pip-keyboard") that take keyboard focus when
    // clicked, so the host lets its window become key/active for them.
    static bool takes_keyboard(Rml::Element* element);

protected:
    const char* model_name() const override;
    void build(rml::Builder& builder) override;

private:
    void request_volume(float value);
};

} // namespace parties::client
