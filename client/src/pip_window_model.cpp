#include <client/pip_window_model.h>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cmath>

namespace parties::client {

namespace {

constexpr float kKeyStep = 0.01f;    // arrows: the slider's own step
constexpr float kPageStep = 0.10f;   // Page Up / Page Down

bool has_class_in_ancestors(Rml::Element* element, const char* name) {
    for (; element; element = element->GetParentNode())
        if (element->IsClassSet(name)) return true;
    return false;
}

} // namespace

bool PipWindowModel::is_action(Rml::Element* element) {
    return has_class_in_ancestors(element, "pip-action");
}

bool PipWindowModel::takes_keyboard(Rml::Element* element) {
    return has_class_in_ancestors(element, "pip-keyboard");
}

const char* PipWindowModel::model_name() const {
    return "pip";
}

void PipWindowModel::request_volume(float value) {
    value = std::clamp(value, 0.0f, kMaxVolume);
    volume = value;
    if (on_volume_changed) on_volume_changed(value);
}

void PipWindowModel::build(rml::Builder& builder) {
    builder.bind("title", title)
        .bind("volume", volume)
        .bind("overlay_visible", overlay_visible);

    builder.on("pip_return", [this] {
        if (on_return) on_return();
    });
    builder.on_event("pip_volume_changed", [this](Rml::Event& event, const Rml::VariantList&) {
        // data-event-change runs before the data-value binding commits, so
        // `volume` still holds the mirrored value. A programmatic update (the
        // host mirroring the application volume) dispatches change with that
        // same value; only a user change differs by at least one step.
        const float value = event.GetParameter<float>("value", volume.get());
        if (std::fabs(value - volume.get()) < kKeyStep * 0.5f) return;
        request_volume(value);
    });
    builder.on_event("pip_volume_key", [this](Rml::Event& event, const Rml::VariantList&) {
        // Left/Right are handled by the slider itself; add the vertical
        // arrows and the usual large-step keys.
        using namespace Rml::Input;
        const auto key = static_cast<KeyIdentifier>(event.GetParameter<int>("key_identifier", KI_UNKNOWN));
        const float current = volume.get();
        float target = current;
        switch (key) {
        case KI_UP:    target = current + kKeyStep; break;
        case KI_DOWN:  target = current - kKeyStep; break;
        case KI_PRIOR: target = current + kPageStep; break;
        case KI_NEXT:  target = current - kPageStep; break;
        case KI_HOME:  target = 0.0f; break;
        case KI_END:   target = kMaxVolume; break;
        default: return;
        }
        // Snap to the slider's step so both controls show the same value.
        target = std::round(target / kKeyStep) * kKeyStep;
        event.StopPropagation();
        if (std::clamp(target, 0.0f, kMaxVolume) != current) request_volume(target);
    });
    builder.on("pip_close", [this] {
        if (on_close) on_close();
    });
}

} // namespace parties::client
