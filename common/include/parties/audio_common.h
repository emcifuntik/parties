#pragma once

#include <algorithm>
#include <cmath>

namespace parties::audio {

constexpr int SAMPLE_RATE      = 48000;
constexpr int CHANNELS         = 1;
constexpr int FRAME_SIZE       = 480;
constexpr int OPUS_FRAME_SIZE  = 960;
constexpr int OPUS_BITRATE     = 40000;
constexpr int OPUS_EXPECTED_LOSS_PCT = 15;
constexpr int MAX_OPUS_PACKET  = 512;

constexpr int SECONDARY_OPUS_BITRATE = 64000;

constexpr float DB_FLOOR = -60.0f;

inline float rms_to_perceptual(float rms) {
    if (rms < 0.001f) return 0.0f;
    float db = 20.0f * std::log10(rms);
    return std::clamp((db - DB_FLOOR) / -DB_FLOOR, 0.0f, 1.0f);
}

inline float perceptual_to_rms(float p) {
    if (p <= 0.0f) return 0.0f;
    float db = p * -DB_FLOOR + DB_FLOOR;
    return std::pow(10.0f, db / 20.0f);
}

inline float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }

constexpr float VOICE_OUTPUT_GAIN_DB = 6.0f;

constexpr float VOLUME_FLOOR_DB     = -40.0f;
constexpr float VOLUME_MAX_BOOST_DB =  6.0f;

inline float volume_position_to_gain(float pos) {
    pos = std::clamp(pos, 0.0f, 2.0f);
    if (pos <= 0.0f) return 0.0f;
    const float db = (pos >= 1.0f)
        ? (pos - 1.0f) * VOLUME_MAX_BOOST_DB
        : (1.0f - pos) * VOLUME_FLOOR_DB;
    return std::pow(10.0f, db / 20.0f);
}

}
