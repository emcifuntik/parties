#include <client/video_frame_router.h>
#include <client/video_element.h>

#include <RmlUi/Core/Element.h>

namespace parties::client {

VideoElement* find_stream_grid_video(Rml::Element* element, UserId stream) {
    if (!element || stream == 0) return nullptr;
    if (element->GetTagName() == "video_frame" &&
        element->GetAttribute<int>("streamid", -1) == static_cast<int>(stream))
        return rmlui_dynamic_cast<VideoElement*>(element);
    const int count = element->GetNumChildren();
    for (int i = 0; i < count; ++i) {
        if (auto* found = find_stream_grid_video(element->GetChild(i), stream))
            return found;
    }
    return nullptr;
}

VideoFrameRouter::Cleared VideoFrameRouter::sync(UserId pip_stream, const Surfaces& surfaces) {
    Cleared cleared;

    // The PiP stream's grid cell must hold nothing. data-if only hides that
    // <video_frame> behind the placeholder, and data-for rebinds cells by
    // index when the watched list changes, so the hidden cell can be one that
    // still holds another stream's texture. Checked on every sync, not only
    // when PiP changes; an empty cell makes this a cheap lookup.
    if (pip_stream != 0) {
        auto* cell = find_stream_grid_video(surfaces.grid, pip_stream);
        if (cell && cell->frame_width() != 0) {
            cell->Clear();
            cleared.grid = true;
        }
    }
    if (pip_stream == pip_stream_) return cleared;

    // The PiP surface never shows a previous stream's last frame: on switch or
    // close it starts empty until the next frame of its stream arrives.
    if (surfaces.pip) {
        surfaces.pip->Clear();
        cleared.pip = true;
    }
    pip_stream_ = pip_stream;
    return cleared;
}

VideoElement* VideoFrameRouter::target(UserId stream, const Surfaces& surfaces) const {
    if (stream == 0) return nullptr;
    if (stream == pip_stream_) return surfaces.pip;
    return find_stream_grid_video(surfaces.grid, stream);
}

} // namespace parties::client
