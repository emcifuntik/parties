#include <client/pip_geometry.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace parties::client {

namespace {

double overlap_area(const PipRect& a, const PipRect& b) {
    const double w = (std::min)(a.x + a.width, b.x + b.width) - (std::max)(a.x, b.x);
    const double h = (std::min)(a.y + a.height, b.y + b.height) - (std::max)(a.y, b.y);
    return (w > 0.0 && h > 0.0) ? w * h : 0.0;
}

bool contains(const PipRect& outer, const PipRect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

double sanitize_aspect(double aspect) {
    return (std::isfinite(aspect) && aspect > 0.0) ? aspect : kPipDefaultAspect;
}

} // namespace

double pip_height_for_width(double width, double aspect) {
    return width / sanitize_aspect(aspect);
}

PipRect pip_fit_aspect(PipRect rect, double aspect) {
    rect.height = pip_height_for_width(rect.width, aspect);
    return rect;
}

PipRect pip_default_rect(const PipRect& work_area, double aspect, double width, double margin) {
    PipRect rect;
    rect.width = width;
    rect.height = pip_height_for_width(width, aspect);
    rect.x = work_area.x + work_area.width - rect.width - margin;
    rect.y = work_area.y + work_area.height - rect.height - margin;
    return rect;
}

int pip_best_work_area(const PipRect& rect, const std::vector<PipRect>& work_areas) {
    int best = -1;
    double best_overlap = 0.0;
    for (size_t i = 0; i < work_areas.size(); ++i) {
        const double area = overlap_area(rect, work_areas[i]);
        if (area > best_overlap) {
            best_overlap = area;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) return best;

    const double cx = rect.x + rect.width * 0.5;
    const double cy = rect.y + rect.height * 0.5;
    double best_distance = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < work_areas.size(); ++i) {
        const PipRect& area = work_areas[i];
        const double dx = cx - (area.x + area.width * 0.5);
        const double dy = cy - (area.y + area.height * 0.5);
        const double distance = dx * dx + dy * dy;
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

PipRect pip_clamp_to_work_areas(PipRect rect, const std::vector<PipRect>& work_areas,
                                const PipSizeLimits& limits) {
    const int index = pip_best_work_area(rect, work_areas);
    if (index < 0) return rect;
    const PipRect& area = work_areas[static_cast<size_t>(index)];

    const double aspect = (rect.width > 0.0 && rect.height > 0.0)
        ? rect.width / rect.height : kPipDefaultAspect;
    if (!(rect.width > 0.0) || !(rect.height > 0.0)) {
        rect.width = limits.min_width;
        rect.height = pip_height_for_width(rect.width, aspect);
    }

    // Size: within [min_width, max_work_fraction of the work area], aspect kept.
    const double max_width = area.width * limits.max_work_fraction;
    const double max_height = area.height * limits.max_work_fraction;
    double scale = 1.0;
    if (rect.width < limits.min_width) scale = limits.min_width / rect.width;
    if (rect.width * scale > max_width) scale = max_width / rect.width;
    if (rect.height * scale > max_height) scale = max_height / rect.height;
    rect.width *= scale;
    rect.height *= scale;

    if (contains(area, rect)) return rect;
    rect.x = std::clamp(rect.x, area.x, area.x + area.width - rect.width);
    rect.y = std::clamp(rect.y, area.y, area.y + area.height - rect.height);
    return rect;
}

std::string pip_rect_to_string(const PipRect& rect) {
    auto number = [](double value) {
        return std::to_string(static_cast<long long>(std::llround(value)));
    };
    return number(rect.x) + "," + number(rect.y) + "," +
           number(rect.width) + "," + number(rect.height);
}

std::optional<PipRect> pip_rect_from_string(std::string_view text) {
    double values[4]{};
    size_t position = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t end = i < 3 ? text.find(',', position) : text.size();
        if (end == std::string_view::npos) return std::nullopt;
        const std::string_view field = text.substr(position, end - position);
        long long value = 0;
        const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
        if (result.ec != std::errc{} || result.ptr != field.data() + field.size() || field.empty())
            return std::nullopt;
        values[i] = static_cast<double>(value);
        position = end + 1;
    }
    if (values[2] <= 0.0 || values[3] <= 0.0) return std::nullopt;
    return PipRect{values[0], values[1], values[2], values[3]};
}

} // namespace parties::client
