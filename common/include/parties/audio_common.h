#pragma once

#include <algorithm>
#include <cmath>

namespace parties::audio {

constexpr int SAMPLE_RATE      = 48000;
constexpr int CHANNELS         = 1;
constexpr int FRAME_SIZE       = 480;     // 10ms at 48kHz (matches RNNoise)
constexpr int OPUS_FRAME_SIZE  = 960;     // 20ms at 48kHz (2x RNNoise frames)
constexpr int OPUS_BITRATE     = 32000;   // 32 kbps default
constexpr int MAX_OPUS_PACKET  = 512;     // max bytes per opus frame

constexpr float DB_FLOOR = -60.0f;        // dB floor for display/VAD mapping

// Linear RMS → perceptual [0,1] (maps [-60dB, 0dB] → [0, 1])
inline float rms_to_perceptual(float rms) {
    if (rms < 0.001f) return 0.0f;
    float db = 20.0f * std::log10(rms);
    return std::clamp((db - DB_FLOOR) / -DB_FLOOR, 0.0f, 1.0f);
}

// Perceptual [0,1] → linear RMS (maps [0, 1] → [-60dB, 0dB])
inline float perceptual_to_rms(float p) {
    if (p <= 0.0f) return 0.0f;
    float db = p * -DB_FLOOR + DB_FLOOR;
    return std::pow(10.0f, db / 20.0f);
}

} // namespace parties::audio
