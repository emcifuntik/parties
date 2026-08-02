#include <client/application_audio.h>

#include <algorithm>
#include <cmath>

namespace parties::client {

void downmix_application_audio(const float* stereo, float* mono, int frame_count) noexcept {
    if (!stereo || !mono || frame_count <= 0)
        return;

    for (int frame = 0; frame < frame_count; ++frame) {
        float left = stereo[frame * 2];
        float right = stereo[frame * 2 + 1];
        // Some application audio graphs briefly emit non-finite samples while
        // changing devices/format. Passing one NaN into the stateful Opus music
        // encoder can poison later frames and sound like permanent white noise.
        left = std::isfinite(left) ? std::clamp(left, -1.0f, 1.0f) : 0.0f;
        right = std::isfinite(right) ? std::clamp(right, -1.0f, 1.0f) : 0.0f;
        mono[frame] = (left + right) * 0.5f;
    }
}

} // namespace parties::client
