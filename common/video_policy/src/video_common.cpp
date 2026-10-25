#include <parties/video_common.h>

#include <cstring>

namespace parties {

void VideoFrameHeader::write(std::span<uint8_t, VIDEO_FRAME_HEADER_SIZE> out) const {
    const uint8_t codec_byte = static_cast<uint8_t>(codec);
    std::memcpy(out.data() + 0, &frame_seq, 4);
    std::memcpy(out.data() + 4, &frame_seq, 4);
    out[8] = keyframe ? VIDEO_FLAG_KEYFRAME : 0;
    std::memcpy(out.data() + 9, &width, 2);
    std::memcpy(out.data() + 11, &height, 2);
    out[13] = codec_byte;
}

std::optional<VideoFrameHeader> VideoFrameHeader::parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < VIDEO_FRAME_HEADER_SIZE) return std::nullopt;
    VideoFrameHeader h;
    std::memcpy(&h.frame_seq, bytes.data() + 0, 4);
    h.keyframe = (bytes[8] & VIDEO_FLAG_KEYFRAME) != 0;
    std::memcpy(&h.width, bytes.data() + 9, 2);
    std::memcpy(&h.height, bytes.data() + 11, 2);
    h.codec = static_cast<VideoCodecId>(bytes[13]);
    return h;
}

std::optional<ParsedVideoFrame> ParsedVideoFrame::parse(std::vector<uint8_t>&& wire) {
    auto header = VideoFrameHeader::parse(wire);
    if (!header) return std::nullopt;
    return ParsedVideoFrame(*header, std::move(wire));
}

ParsedVideoFrame ParsedVideoFrame::assemble(const VideoFrameHeader& header,
                                            std::span<const uint8_t> encoded) {
    std::vector<uint8_t> wire(VIDEO_FRAME_HEADER_SIZE + encoded.size());
    header.write(std::span<uint8_t, VIDEO_FRAME_HEADER_SIZE>(wire.data(), VIDEO_FRAME_HEADER_SIZE));
    if (!encoded.empty())
        std::memcpy(wire.data() + VIDEO_FRAME_HEADER_SIZE, encoded.data(), encoded.size());
    return ParsedVideoFrame(header, std::move(wire));
}

}
