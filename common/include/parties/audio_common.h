#pragma once

#include <algorithm>
#include <cmath>

namespace parties::audio {

constexpr int SAMPLE_RATE      = 48000;
constexpr int CHANNELS         = 1;
constexpr int FRAME_SIZE       = 480;     // 10ms at 48kHz (matches RNNoise)
constexpr int OPUS_FRAME_SIZE  = 960;     // 20ms at 48kHz (2x RNNoise frames)
constexpr int OPUS_BITRATE     = 40000;   // 40 kbps: +8k over the old 32k funds the
                                          // in-band FEC (LBRR) copy so primary quality
                                          // stays ~32k-equivalent under loss
constexpr int OPUS_EXPECTED_LOSS_PCT = 15; // size LBRR for the 10% target with burst headroom
constexpr int MAX_OPUS_PACKET  = 512;     // max bytes per opus frame (~100B at 40kbps/20ms)

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

// dB → linear amplitude gain.
inline float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }

// Master makeup gain applied to the mixed received voice. Decoded voice sits
// well below full scale, so at unity per-user volume everyone is too quiet;
// this raises the whole mix to a comfortable default. The per-user volume
// slider trims on top of this. Tune here if voices are too quiet/loud.
constexpr float VOICE_OUTPUT_GAIN_DB = 6.0f;   // +6 dB ≈ 2x

// Volume control: bottom-of-slider attenuation and top-of-slider boost (dB).
constexpr float VOLUME_FLOOR_DB     = -40.0f;  // pos→0 attenuation before mute
constexpr float VOLUME_MAX_BOOST_DB =  6.0f;   // +6 dB ≈ 2x amplitude at pos 2.0

// Map a volume-slider position (0..2, where 1.0 = unity / 0 dB) to a linear
// amplitude gain along a perceptual (dB) curve. Loudness perception is roughly
// logarithmic, so multiplying samples by the raw slider position makes the
// control feel dead across its top half and crammed near the bottom. Here equal
// slider movements yield roughly equal loudness (dB) changes instead:
//   pos 0    → 0      (true mute)
//   pos 0..1 → -40 dB .. 0 dB
//   pos 1    → unity  (0 dB)
//   pos 1..2 → 0 dB .. +6 dB
inline float volume_position_to_gain(float pos) {
    pos = std::clamp(pos, 0.0f, 2.0f);
    if (pos <= 0.0f) return 0.0f;
    const float db = (pos >= 1.0f)
        ? (pos - 1.0f) * VOLUME_MAX_BOOST_DB
        : (1.0f - pos) * VOLUME_FLOOR_DB;
    return std::pow(10.0f, db / 20.0f);
}

} // namespace parties::audio
