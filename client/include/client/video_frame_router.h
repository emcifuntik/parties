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

class VideoFrameRouter {
public:
    struct Surfaces {
        Rml::Element* grid = nullptr;
        VideoElement* pip = nullptr;
    };
    struct Cleared {
        bool grid = false;
        bool pip = false;
    };

    Cleared sync(UserId pip_stream, const Surfaces& surfaces);

    VideoElement* target(UserId stream, const Surfaces& surfaces) const;

    UserId pip_stream() const { return pip_stream_; }

private:
    UserId pip_stream_ = 0;
};

}
