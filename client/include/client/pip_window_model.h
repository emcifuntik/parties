#pragma once

#include <client/rml_binding.h>

#include <functional>

namespace Rml { class Element; }

namespace parties::client {

class PipWindowModel final : public rml::Model {
public:
    static constexpr float kMaxVolume = 2.0f;

    rml::Prop<Rml::String> title;
    rml::Prop<float> volume{1.0f};
    rml::Prop<bool> overlay_visible{false};

    std::function<void()> on_return;
    std::function<void(float)> on_volume_changed;
    std::function<void()> on_close;

    static bool is_action(Rml::Element* element);
    static bool takes_keyboard(Rml::Element* element);

protected:
    const char* model_name() const override;
    void build(rml::Builder& builder) override;

private:
    void request_volume(float value);
};

}
