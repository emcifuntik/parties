#include <client/context_window_model.h>

#include <cmath>
#include <string>

namespace parties::client {

const char* ContextWindowModel::model_name() const {
    return "context_window";
}

void ContextWindowModel::build(rml::Builder& builder) {
    builder.register_struct<ContextWindowAction>([](auto& item) {
        item.member("label", &ContextWindowAction::label)
            .member("detail", &ContextWindowAction::detail)
            .member("id", &ContextWindowAction::id)
            .member("danger", &ContextWindowAction::danger)
            .member("separator", &ContextWindowAction::separator);
    });
    builder.register_array<Rml::Vector<ContextWindowAction>>();

    builder.bind("user_mode", user_mode)
        .bind("title", title)
        .bind("subtitle", subtitle)
        .bind("icon_text", icon_text)
        .bind("room_icon", room_icon)
        .bind("role_label", role_label)
        .bind("has_actions", has_actions)
        .bind("volume", volume)
        .bind("volume_text", volume_text)
        .bind("music_volume", music_volume)
        .bind("music_volume_text", music_volume_text)
        .bind("compression", compression)
        .bind("compression_target", compression_target)
        .bind("actions", actions);

    builder.on_args<int>("choose_action", [this](int id) {
        if (on_action) on_action(id);
    });
    builder.on("volume_changed", [this] {
        volume_text = std::to_string(static_cast<int>(std::lround(volume.get() * 100.0f))) + "%";
        if (on_volume) on_volume(volume.get());
    });
    builder.on("music_volume_changed", [this] {
        music_volume_text = std::to_string(
            static_cast<int>(std::lround(music_volume.get() * 100.0f))) + "%";
        if (on_music_volume) on_music_volume(music_volume.get());
    });
    builder.on("toggle_compression", [this] {
        compression = !compression.get();
        if (on_compression) on_compression(compression.get(), compression_target.get());
    });
    builder.on("compression_target_changed", [this] {
        if (on_compression) on_compression(compression.get(), compression_target.get());
    });
    builder.on("close_context_window", [this] {
        if (on_close) on_close();
    });
}

} // namespace parties::client
