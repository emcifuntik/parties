#pragma once

#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>

namespace parties::client {

enum class IOSBackAction { None, ActivateControl, ExitFullscreen };
struct IOSBackTarget {
    IOSBackAction action = IOSBackAction::None;
    Rml::Element* control = nullptr;
};

inline IOSBackTarget FindIOSBackTarget(Rml::ElementDocument* document,
    bool connected, bool showing_content, bool fullscreen, bool native_overlay)
{
    if (!document || !connected || !showing_content || native_overlay) return {};
    Rml::ElementList overlays;
    document->QuerySelectorAll(overlays, ".modal-backdrop, .user-menu-backdrop");
    for (auto* overlay : overlays)
        if (overlay->IsVisible(true)) return {};
    if (fullscreen) {
        auto* viewer = document->QuerySelector(".screen-share-area.fullscreen");
        if (viewer && viewer->IsVisible(true)) return {IOSBackAction::ExitFullscreen};
    }
    // Use the page's own back action, including the voice room and stream viewer.
    for (const char* selector : {".mobile-back-bar", ".stream-mobile-back"}) {
        auto* control = document->QuerySelector(selector);
        if (control && control->IsVisible(true)) return {IOSBackAction::ActivateControl, control};
    }
    return {};
}

inline int IOSViewportHeight(float height, float top, float keyboard, float scale)
{
    // Page backgrounds extend to the screen edge; only the keyboard reduces the viewport.
    return std::max(1, static_cast<int>(std::floor(height * scale)) -
        static_cast<int>(std::ceil(top * scale)) -
        static_cast<int>(std::ceil(keyboard * scale)));
}

inline void ApplyIOSSafeArea(Rml::ElementDocument* document, float left, float right, float bottom, float keyboard = 0)
{
    if (!document) return;
    const auto dp = [](float value) { return Rml::String(std::to_string(value) + "dp"); };
    // ElementDocument is the RML body; there is no nested body child to search.
    document->SetProperty("padding-left", dp(left));
    document->SetProperty("padding-right", dp(right));
    document->SetProperty("--safe-area-left", dp(left));
    document->SetProperty("--safe-area-right", dp(right));
    // Only floating bottom controls consume this inset. The page itself stays full height.
    document->SetProperty("--safe-area-bottom", dp(std::max(0.f, bottom - keyboard)));
}

} // namespace parties::client
