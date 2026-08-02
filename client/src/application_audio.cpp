#include <client/application_audio.h>

namespace parties::client {

void downmix_application_audio(const float* stereo, float* mono, int frame_count) noexcept {
    if (!stereo || !mono || frame_count <= 0)
        return;

    for (int frame = 0; frame < frame_count; ++frame)
        mono[frame] = (stereo[frame * 2] + stereo[frame * 2 + 1]) * 0.5f;
}

} // namespace parties::client
