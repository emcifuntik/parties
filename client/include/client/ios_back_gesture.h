#pragma once

#include <algorithm>
#include <cmath>

namespace parties::client {

inline bool IsIOSBackSwipeDirection(float horizontal, float vertical)
{
    return horizontal > 0.0f && horizontal > std::abs(vertical) * 1.2f;
}

// UIKit recognizes the screen edge. Commit only an intentional completed pan;
// cancelled recognizers never reach this policy. Distances are UIKit points.
inline bool ShouldCompleteIOSBackSwipe(float horizontal, float vertical,
    float velocity, float width)
{
    if (!IsIOSBackSwipeDirection(horizontal, vertical) || velocity < -100.0f)
        return false;
    const float distance = std::clamp(width * 0.25f, 64.0f, 120.0f);
    return horizontal >= distance || (horizontal >= 24.0f && velocity >= 650.0f);
}

} // namespace parties::client
