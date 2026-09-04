#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace parties {

enum class VideoCodecId : uint8_t {
    AV1  = 0x01,
    H265 = 0x02,
    H264 = 0x03,
};

// Video frame flags (bitfield)
constexpr uint8_t VIDEO_FLAG_KEYFRAME = 0x01;

// Bitrate limits (bits per second)
constexpr uint32_t VIDEO_MAX_BITRATE       = 20'000'000;  // 20 Mbps
constexpr uint32_t VIDEO_DEFAULT_BITRATE   = 2'000'000;   // 2 Mbps
constexpr uint32_t VIDEO_MIN_BITRATE       =   200'000;   // 200 kbps (UI floor)
// Floor for automatic congestion adaptation. Below this a 1080p screen share
// is unreadable, so the sender drops frames instead of going lower.
constexpr uint32_t VIDEO_ADAPT_MIN_BITRATE =   800'000;   // 800 kbps

// Timing
constexpr uint32_t VIDEO_KEYFRAME_INTERVAL_MS = 5000;  // Max 5 seconds between keyframes
constexpr uint32_t VIDEO_PLI_COOLDOWN_MS      =  500;  // Min time between PLI sends per target
constexpr uint32_t VIDEO_PLI_RETRY_MS         =  500;  // Re-send PLI while still waiting for a keyframe
constexpr uint32_t VIDEO_KEYFRAME_REQUEST_COOLDOWN_MS = 500;  // Min time between forced keyframes on the sharer

// Largest on-wire video record accepted by any stream parser: the length
// prefix / type byte plus everything after it ([type][sender][header][encoded]).
constexpr size_t VIDEO_FRAME_MAX_BYTES = 4u * 1024u * 1024u;
// Largest [header][encoded] a sharer may send. The server prepends up to
// 1 (type) + 4 (sender_id) bytes when re-originating a frame, so the sharer's
// limit is 5 bytes below the parser cap on every path; every ingress enforces
// this value so a compliant frame can never be rejected downstream.
constexpr size_t VIDEO_FRAME_MAX_PAYLOAD_BYTES = VIDEO_FRAME_MAX_BYTES - 1 - 4;

// ── Frame header ─────────────────────────────────────────────────────────────
// Common 14-byte header of every screen-share frame, identical on the legacy
// video stream 1 ([0x02][sender][hdr][encoded] after server forwarding) and on
// per-frame unidirectional streams ([0x12][sender][hdr][encoded]).
//   [frame_seq u32][timestamp u32][flags u8][width u16][height u16][codec u8]
// All little-endian. timestamp currently mirrors frame_seq; nothing consumes it.
constexpr size_t VIDEO_FRAME_HEADER_SIZE = 14;

struct VideoFrameHeader {
    uint32_t frame_seq = 0;
    uint32_t timestamp = 0;
    uint8_t  flags     = 0;
    uint16_t width     = 0;
    uint16_t height    = 0;
    uint8_t  codec     = 0;

    bool keyframe() const { return (flags & VIDEO_FLAG_KEYFRAME) != 0; }

    // Writes exactly VIDEO_FRAME_HEADER_SIZE bytes.
    void write(uint8_t* out) const {
        std::memcpy(out + 0, &frame_seq, 4);
        std::memcpy(out + 4, &timestamp, 4);
        out[8] = flags;
        std::memcpy(out + 9,  &width,  2);
        std::memcpy(out + 11, &height, 2);
        out[13] = codec;
    }

    // Parses the first VIDEO_FRAME_HEADER_SIZE bytes; false if len is too short.
    static bool parse(const uint8_t* data, size_t len, VideoFrameHeader& out) {
        if (!data || len < VIDEO_FRAME_HEADER_SIZE) return false;
        std::memcpy(&out.frame_seq, data + 0, 4);
        std::memcpy(&out.timestamp, data + 4, 4);
        out.flags = data[8];
        std::memcpy(&out.width,  data + 9,  2);
        std::memcpy(&out.height, data + 11, 2);
        out.codec = data[13];
        return true;
    }
};

// Wrap-safe sequence comparison: true when `a` is newer than `b`.
constexpr bool video_seq_newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}
constexpr int32_t video_seq_distance(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

// Nonce space: video counters have the high bit set to avoid collision with voice
constexpr uint64_t VIDEO_NONCE_HIGH_BIT = 0x8000000000000000ULL;

} // namespace parties
