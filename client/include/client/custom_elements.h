#pragma once

#include <client/rml_elements.h>
#include <client/video_element.h>
#include <client/level_meter_element.h>
#include <client/gradient_circle_element.h>
#include <client/chat_selection.h>

namespace parties::client {

// Registers the project's custom RmlUi elements on the given registry. Call once
// per process, before loading any document, on every platform. The registry must
// outlive all documents (RmlUi's Factory keeps raw pointers to the instancers).
inline void register_custom_elements(parties::rml::ElementRegistry& registry) {
    registry.add<VideoElement>("video_frame");
    registry.add<LevelMeterElement>("level_meter");
    registry.add<GradientCircleElement>("gradient_circle");
    registry.add<SelectableTextElement>("selectable_text");
}

} // namespace parties::client
