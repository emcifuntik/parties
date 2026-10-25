#pragma once

namespace parties::client {

void downmix_application_audio(const float* stereo, float* mono, int frame_count) noexcept;

}
