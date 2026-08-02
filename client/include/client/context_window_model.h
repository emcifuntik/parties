#pragma once

#include <client/rml_binding.h>

#include <functional>

namespace parties::client {

struct ContextWindowAction {
    Rml::String label;
    Rml::String detail;
    int id = 0;
    bool danger = false;
    bool separator = false;
};

// Data model shared by the native context-window host and RmlUI Designer.
// Keeping it platform-neutral makes the exact production document available
// to screenshot tests instead of maintaining a separate HTML-like mock.
class ContextWindowModel final : public rml::Model {
public:
    rml::Prop<bool> user_mode{false};
    rml::Prop<Rml::String> title;
    rml::Prop<Rml::String> subtitle;
    rml::Prop<Rml::String> icon_text;
    rml::Prop<bool> room_icon{false};
    rml::Prop<Rml::String> role_label;
    rml::Prop<bool> has_actions{false};
    rml::Prop<float> volume{1.0f};
    rml::Prop<Rml::String> volume_text{"100%"};
    rml::Prop<float> music_volume{1.0f};
    rml::Prop<Rml::String> music_volume_text{"100%"};
    rml::Prop<bool> compression{false};
    rml::Prop<float> compression_target{0.8f};
    rml::Prop<Rml::Vector<ContextWindowAction>> actions;

    std::function<void(int)> on_action;
    std::function<void(float)> on_volume;
    std::function<void(float)> on_music_volume;
    std::function<void(bool, float)> on_compression;
    std::function<void()> on_close;

protected:
    const char* model_name() const override;
    void build(rml::Builder& builder) override;
};

} // namespace parties::client
