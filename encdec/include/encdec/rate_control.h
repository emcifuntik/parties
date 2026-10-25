#pragma once

#include <cstdint>
#include <limits>

namespace parties::encdec {

struct VbrRateControl {
    uint32_t average_bitrate = 0;
    uint32_t peak_bitrate = 0;
    uint32_t vbv_buffer_size = 0;
    uint32_t vbv_initial_delay = 0;
    uint32_t max_frame_bits = 0;
};

constexpr uint32_t STREAM_VBV_MIN_BITS = 250'000;
constexpr uint32_t STREAM_VBV_FRAMES   = 4;
constexpr uint32_t STREAM_MAX_FRAME_FRAMES = 6;

constexpr uint32_t saturate_u32(uint64_t v) {
    return v > (std::numeric_limits<uint32_t>::max)()
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(v);
}

constexpr VbrRateControl make_stream_vbr_rate_control(uint32_t average_bitrate, uint32_t fps) {
    const uint32_t safe_fps = fps == 0 ? 30u : fps;
    const uint64_t avg = average_bitrate;
    const uint32_t peak = saturate_u32(avg * 3u / 2u);
    const uint64_t per_frame = avg / safe_fps;
    uint32_t vbv = saturate_u32(per_frame * STREAM_VBV_FRAMES);
    if (vbv < STREAM_VBV_MIN_BITS) vbv = STREAM_VBV_MIN_BITS;
    const uint32_t max_frame = saturate_u32(per_frame * STREAM_MAX_FRAME_FRAMES);
    return {average_bitrate, peak, vbv, vbv,
            max_frame < vbv ? vbv : max_frame};
}

}
