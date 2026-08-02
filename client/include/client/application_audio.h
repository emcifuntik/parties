#pragma once

namespace parties::client {

// Converts interleaved stereo application-loopback samples to the mono format
// consumed by AudioEngine's secondary VOICE2 encoder.
void downmix_application_audio(const float* stereo, float* mono, int frame_count) noexcept;

} // namespace parties::client
