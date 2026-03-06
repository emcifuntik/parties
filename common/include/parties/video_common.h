#pragma once

#include <cstdint>

namespace parties {

enum class VideoCodecId : uint8_t {
    AV1  = 0x01,
    H265 = 0x02,
    H264 = 0x03,
};

// Video frame flags (bitfield)
constexpr uint8_t VIDEO_FLAG_KEYFRAME = 0x01;

// Bitrate limits (bits per second)
constexpr uint32_t VIDEO_MAX_BITRATE     = 10'000'000;  // 10 Mbps
constexpr uint32_t VIDEO_DEFAULT_BITRATE = 2'000'000;  // 2 Mbps
constexpr uint32_t VIDEO_MIN_BITRATE     =   200'000;  // 200 kbps

// Timing
constexpr uint32_t VIDEO_KEYFRAME_INTERVAL_MS = 5000;  // Max 5 seconds between keyframes
constexpr uint32_t VIDEO_PLI_COOLDOWN_MS      =  500;  // Min time between PLI sends

// Nonce space: video counters have the high bit set to avoid collision with voice
constexpr uint64_t VIDEO_NONCE_HIGH_BIT = 0x8000000000000000ULL;

} // namespace parties
