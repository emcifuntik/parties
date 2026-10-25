#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace parties {

enum class VideoCodecId : uint8_t {
    AV1  = 0x01,
    H265 = 0x02,
    H264 = 0x03,
};

constexpr uint8_t VIDEO_FLAG_KEYFRAME = 0x01;

constexpr uint32_t VIDEO_MAX_BITRATE       = 20'000'000;
constexpr uint32_t VIDEO_DEFAULT_BITRATE   = 2'000'000;
constexpr uint32_t VIDEO_MIN_BITRATE       =   200'000;
constexpr uint32_t VIDEO_ADAPT_MIN_BITRATE =   800'000;

constexpr uint32_t VIDEO_KEYFRAME_INTERVAL_MS = 5000;
constexpr uint32_t VIDEO_PLI_COOLDOWN_MS      =  500;
constexpr uint32_t VIDEO_PLI_RETRY_MS         =  500;
constexpr uint32_t VIDEO_KEYFRAME_REQUEST_COOLDOWN_MS = 500;

constexpr size_t VIDEO_FRAME_MAX_BYTES = 4u * 1024u * 1024u;
constexpr size_t VIDEO_RELAY_PREFIX_BYTES = 1 + 4;
constexpr size_t VIDEO_FRAME_MAX_PAYLOAD_BYTES = VIDEO_FRAME_MAX_BYTES - VIDEO_RELAY_PREFIX_BYTES;

constexpr size_t VIDEO_FRAME_HEADER_SIZE = 14;

struct VideoFrameHeader {
    uint32_t     frame_seq = 0;
    bool         keyframe  = false;
    uint16_t     width     = 0;
    uint16_t     height    = 0;
    VideoCodecId codec     = VideoCodecId::AV1;

    void write(std::span<uint8_t, VIDEO_FRAME_HEADER_SIZE> out) const;
    static std::optional<VideoFrameHeader> parse(std::span<const uint8_t> bytes);
};

class ParsedVideoFrame {
public:
    static std::optional<ParsedVideoFrame> parse(std::vector<uint8_t>&& wire);
    static ParsedVideoFrame assemble(const VideoFrameHeader& header, std::span<const uint8_t> encoded);

    const VideoFrameHeader&  header() const { return header_; }
    std::span<const uint8_t> wire() const { return wire_; }
    std::span<const uint8_t> encoded() const { return std::span(wire_).subspan(VIDEO_FRAME_HEADER_SIZE); }
    size_t                   wire_size() const { return wire_.size(); }

    ParsedVideoFrame(ParsedVideoFrame&&) noexcept = default;
    ParsedVideoFrame& operator=(ParsedVideoFrame&&) noexcept = default;
    ParsedVideoFrame(const ParsedVideoFrame&) = delete;
    ParsedVideoFrame& operator=(const ParsedVideoFrame&) = delete;

private:
    ParsedVideoFrame(const VideoFrameHeader& header, std::vector<uint8_t>&& wire)
        : header_(header), wire_(std::move(wire)) {}

    VideoFrameHeader     header_;
    std::vector<uint8_t> wire_;
};

constexpr bool video_seq_newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}
constexpr int32_t video_seq_distance(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

}
