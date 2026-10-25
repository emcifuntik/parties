#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parties::client {

struct PipRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const PipRect&) const = default;
};

struct PipSizeLimits {
    double min_width = 192.0;
    double max_work_fraction = 0.9;
};

constexpr double kPipDefaultAspect = 16.0 / 9.0;

double pip_height_for_width(double width, double aspect);

PipRect pip_fit_aspect(PipRect rect, double aspect);

PipRect pip_default_rect(const PipRect& work_area, double aspect, double width, double margin);

int pip_best_work_area(const PipRect& rect, const std::vector<PipRect>& work_areas);

PipRect pip_clamp_to_work_areas(PipRect rect, const std::vector<PipRect>& work_areas,
                                const PipSizeLimits& limits = {});

std::string pip_rect_to_string(const PipRect& rect);
std::optional<PipRect> pip_rect_from_string(std::string_view text);

}
