#pragma once

#include <RmlUi/Core/SystemInterface.h>

/**
 * iOS system interface for RmlUi.
 *
 * Provides the minimum required platform functionality:
 *   - GetElapsedTime()  (required)
 *   - Clipboard access via UIPasteboard
 *   - Keyboard activation for text fields
 */
class SystemInterface_iOS : public Rml::SystemInterface {
public:
    SystemInterface_iOS();
    ~SystemInterface_iOS() override;

    // ---- Required ----
    double GetElapsedTime() override;

    // ---- Optional (logging, clipboard, keyboard) ----
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;

    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
    void DeactivateKeyboard() override;
};
