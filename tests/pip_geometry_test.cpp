// Remembered picture-in-picture geometry is always placed fully on a visible
// work area, keeps its aspect ratio, and round-trips through its preference.

#include <client/pip_geometry.h>

#include <cmath>
#include <cstdio>

namespace {

using parties::client::PipRect;

bool passed = true;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "PiP geometry: %s\n", message);
        passed = false;
    }
}

bool inside(const PipRect& outer, const PipRect& inner) {
    constexpr double e = 1e-6;
    return inner.x >= outer.x - e && inner.y >= outer.y - e &&
           inner.x + inner.width <= outer.x + outer.width + e &&
           inner.y + inner.height <= outer.y + outer.height + e;
}

bool same_aspect(const PipRect& a, const PipRect& b) {
    return std::fabs(a.width / a.height - b.width / b.height) < 1e-6;
}

} // namespace

int main() {
    using namespace parties::client;

    // Primary 1920x1040 work area (taskbar excluded) and a 1280x1024 monitor
    // to its right whose work area starts lower.
    const PipRect primary{0, 0, 1920, 1040};
    const PipRect secondary{1920, 100, 1280, 1024};
    const std::vector<PipRect> both{primary, secondary};
    const std::vector<PipRect> only_primary{primary};

    // A rect that is already fully visible keeps its exact position and size.
    const PipRect visible{1500, 700, 384, 216};
    check(pip_clamp_to_work_areas(visible, both) == visible, "visible rect was moved");
    const PipRect on_secondary{2600, 800, 400, 225};
    check(pip_clamp_to_work_areas(on_secondary, both) == on_secondary,
        "rect on the secondary monitor was moved");

    // Remembered on a monitor that is now disconnected: moves onto the nearest
    // visible work area, fully, with its size and aspect ratio intact.
    const PipRect stranded = pip_clamp_to_work_areas(on_secondary, only_primary);
    check(inside(primary, stranded), "rect from a disconnected monitor is not fully visible");
    check(stranded.width == on_secondary.width && stranded.height == on_secondary.height,
        "rect from a disconnected monitor changed size");

    // Partially off the right edge, and straddling the taskbar.
    const PipRect off_edge = pip_clamp_to_work_areas({1800, 500, 400, 225}, only_primary);
    check(inside(primary, off_edge) && off_edge.y == 500, "off-edge rect was not pulled inside");
    const PipRect under_taskbar = pip_clamp_to_work_areas({200, 950, 400, 225}, only_primary);
    check(inside(primary, under_taskbar) && under_taskbar.x == 200,
        "rect under the taskbar was not pulled inside");

    // Straddling two monitors: lands entirely on the one it overlaps most.
    const PipRect straddle = pip_clamp_to_work_areas({1700, 400, 400, 225}, both);
    check(inside(secondary, straddle) || inside(primary, straddle),
        "rect straddling two monitors is not fully on one");
    check(inside(primary, straddle), "rect did not stay on the monitor it overlapped most");
    const PipRect mostly_right = pip_clamp_to_work_areas({1880, 400, 400, 225}, both);
    check(inside(secondary, mostly_right), "rect did not move to the monitor it overlapped most");

    // Far outside every monitor (negative coordinates of an old layout).
    const PipRect far = pip_clamp_to_work_areas({-5000, -3000, 400, 225}, both);
    check(inside(primary, far), "far-away rect was not placed on the nearest monitor");

    // Larger than the work area: shrunk with its aspect ratio, then placed.
    const PipRect huge{100, 100, 4000, 2250};
    const PipRect shrunk = pip_clamp_to_work_areas(huge, only_primary);
    check(inside(primary, shrunk) && same_aspect(huge, shrunk), "oversized rect lost its aspect ratio");
    check(shrunk.width <= primary.width * 0.9 + 1e-6, "oversized rect was not limited");

    // Too small to use: grown to the minimum width, aspect kept.
    const PipRect tiny = pip_clamp_to_work_areas({10, 10, 40, 30}, only_primary);
    check(tiny.width >= PipSizeLimits{}.min_width - 1e-6 && std::fabs(tiny.width / tiny.height - 4.0 / 3.0) < 1e-6,
        "tiny rect was not grown with its aspect ratio");

    // No monitors reported: unchanged rather than invented.
    check(pip_clamp_to_work_areas(visible, {}) == visible, "rect changed without work areas");

    // Default placement: bottom-right, inside the work area, at the aspect.
    const PipRect placed = pip_default_rect(primary, 16.0 / 9.0, 384, 24);
    check(inside(primary, placed) && placed.x + placed.width == 1896 && placed.y + placed.height == 1016 &&
          std::fabs(placed.height - 216.0) < 1e-6, "default placement is wrong");

    // Aspect fitting keeps width and the top-left corner.
    const PipRect portrait = pip_fit_aspect({50, 60, 300, 100}, 9.0 / 16.0);
    check(portrait.x == 50 && portrait.y == 60 && portrait.width == 300 &&
          std::fabs(portrait.height - 533.333333) < 1e-3, "aspect fit is wrong");
    check(std::fabs(pip_height_for_width(320, 0.0) - 180.0) < 1e-6, "invalid aspect did not fall back to 16:9");

    // Preference round trip and rejection of corrupted values.
    const auto parsed = pip_rect_from_string(pip_rect_to_string({-1280, 24, 480, 270}));
    check(parsed && *parsed == PipRect{-1280, 24, 480, 270}, "rect did not round-trip");
    check(!pip_rect_from_string("") && !pip_rect_from_string("1,2,3") &&
          !pip_rect_from_string("1,2,0,4") && !pip_rect_from_string("1,2,3,x") &&
          !pip_rect_from_string("1,2,3,4,5") && !pip_rect_from_string("1,,3,4"),
        "malformed preferences were accepted");

    return passed ? 0 : 1;
}
