#pragma once

#include <parties/types.h>

namespace Rml { class Element; }

namespace parties::client {

class VideoElement;

// The <video_frame> grid cell whose bound "streamid" attribute is `stream`,
// searched below `grid` (the #stream-grid element), or null. The cells are
// created by a data-for binding, so the attribute is read rather than relying
// on GetElementById finding a data-bound id.
VideoElement* find_stream_grid_video(Rml::Element* grid, UserId stream);

// Chooses the one surface that receives a stream's decoded frames: the PiP
// surface while that stream is in picture-in-picture, its grid cell otherwise.
// A frame is never delivered to both.
//
// sync() clears the surface each stream leaves when the PiP stream changes,
// and keeps the PiP stream's (hidden) grid cell empty while PiP stays open.
// VideoElement::Clear releases its textures through the render interface that
// created them, so a renderer's retirement rules (DX12 back-buffer fences,
// decoder leases) apply to exactly the resources it allocated.
//
// Render-thread (or main-thread on Apple) object; not thread-safe.
class VideoFrameRouter {
public:
    struct Surfaces {
        Rml::Element* grid = nullptr;   // #stream-grid in the main document
        VideoElement* pip = nullptr;    // RmlUi PiP surface (desktop), if it exists
    };
    struct Cleared {
        bool grid = false;   // a grid cell released its video resources
        bool pip = false;    // the PiP surface released its video resources
    };

    // Call before delivering frames, with the controller's current PiP stream.
    Cleared sync(UserId pip_stream, const Surfaces& surfaces);

    // Destination for `stream`'s next frame, or null (the frame is dropped,
    // e.g. its surface does not exist yet). Uses the stream last passed to sync.
    VideoElement* target(UserId stream, const Surfaces& surfaces) const;

    UserId pip_stream() const { return pip_stream_; }

private:
    UserId pip_stream_ = 0;
};

} // namespace parties::client
