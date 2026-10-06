#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parties::client {

// Desktop picture-in-picture window geometry. Units are whatever the platform
// uses for window placement (Win32 virtual-screen pixels, AppKit points); the
// math only needs one consistent coordinate system.
struct PipRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const PipRect&) const = default;
};

struct PipSizeLimits {
    double min_width = 192.0;        // smallest usable window (overlay buttons still fit)
    double max_work_fraction = 0.9;  // never larger than this share of a work area
};

constexpr double kPipDefaultAspect = 16.0 / 9.0;

// Height for `width` at `aspect` (width / height). Non-positive aspects fall
// back to 16:9.
double pip_height_for_width(double width, double aspect);

// Resize `rect` to `aspect`, keeping its width and top-left corner.
PipRect pip_fit_aspect(PipRect rect, double aspect);

// First-open placement: bottom-right of `work_area`, `width` wide, `margin`
// away from the edges.
PipRect pip_default_rect(const PipRect& work_area, double aspect, double width, double margin);

// Index of the work area a window belongs to: the one it overlaps most, else
// the one whose centre is nearest to the window's centre. -1 when empty.
int pip_best_work_area(const PipRect& rect, const std::vector<PipRect>& work_areas);

// Place `rect` fully inside a visible work area. It keeps its position when it
// already fits; otherwise it is scaled down (keeping its aspect ratio) to the
// limits and moved inside the work area chosen by pip_best_work_area. With no
// work areas the rect is returned unchanged.
PipRect pip_clamp_to_work_areas(PipRect rect, const std::vector<PipRect>& work_areas,
                                const PipSizeLimits& limits = {});

// Persistence format for the remembered window rect: "x,y,width,height".
std::string pip_rect_to_string(const PipRect& rect);
// nullopt for malformed input or a non-positive size.
std::optional<PipRect> pip_rect_from_string(std::string_view text);

} // namespace parties::client
