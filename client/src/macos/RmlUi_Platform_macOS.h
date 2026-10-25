#pragma once

#include <RmlUi/Core/SystemInterface.h>

class SystemInterface_macOS : public Rml::SystemInterface {
public:
    SystemInterface_macOS();
    ~SystemInterface_macOS() override;

    double GetElapsedTime() override;

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;

    // No-op on macOS — AppKit handles keyboard via NSResponder.
    void ActivateKeyboard(Rml::Vector2f, float) override {}
    void DeactivateKeyboard() override {}
};
