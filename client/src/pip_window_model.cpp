#include <client/pip_window_model.h>

namespace parties::client {

const char* PipWindowModel::model_name() const {
    return "pip";
}

void PipWindowModel::build(rml::Builder& builder) {
    builder.bind("title", title)
        .bind("muted", muted)
        .bind("overlay_visible", overlay_visible);

    builder.on("pip_return", [this] {
        if (on_return) on_return();
    });
    builder.on("pip_toggle_mute", [this] {
        if (on_toggle_mute) on_toggle_mute();
    });
    builder.on("pip_close", [this] {
        if (on_close) on_close();
    });
}

} // namespace parties::client
