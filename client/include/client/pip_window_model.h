#pragma once

#include <client/rml_binding.h>

#include <functional>

namespace parties::client {

// Data model of the desktop picture-in-picture document (ui/pip.rml), shared
// by the Windows and macOS PiP hosts. The host owns the window; this model only
// carries what the overlay shows and the three overlay actions.
class PipWindowModel final : public rml::Model {
public:
    rml::Prop<Rml::String> title;
    rml::Prop<bool> muted{false};
    // Set by the host while the cursor is over the window. Nothing is shown
    // otherwise.
    rml::Prop<bool> overlay_visible{false};

    std::function<void()> on_return;       // back to the main window
    std::function<void()> on_toggle_mute;  // stream audio
    std::function<void()> on_close;        // close PiP, keep watching in the grid

protected:
    const char* model_name() const override;
    void build(rml::Builder& builder) override;
};

} // namespace parties::client
