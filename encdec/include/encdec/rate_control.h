#pragma once

#include <cstdint>
#include <limits>

namespace parties::encdec {

struct VbrRateControl {
    uint32_t average_bps = 0;
    uint32_t peak_bps = 0;
    uint32_t vbv_buffer_bits = 0;
    uint32_t vbv_initial_delay_bits = 0;
    uint32_t max_frame_bits = 0;
};

constexpr uint32_t STREAM_VBV_MIN_BITS = 250'000;
constexpr uint32_t STREAM_VBV_FRAMES   = 4;
constexpr uint32_t STREAM_MAX_FRAME_FRAMES = 6;
constexpr uint32_t PEAK_TO_AVERAGE_NUMERATOR = 3;
constexpr uint32_t PEAK_TO_AVERAGE_DENOMINATOR = 2;

constexpr uint32_t saturate_u32(uint64_t v) {
    return v > (std::numeric_limits<uint32_t>::max)()
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(v);
}

constexpr uint32_t peak_bps_for(uint32_t average_bps) {
    return saturate_u32(uint64_t{average_bps} * PEAK_TO_AVERAGE_NUMERATOR /
                        PEAK_TO_AVERAGE_DENOMINATOR);
}

constexpr VbrRateControl make_stream_vbr_rate_control(uint32_t average_bps, uint32_t fps) {
    const uint32_t safe_fps = fps == 0 ? 30u : fps;
    const uint64_t per_frame_bits = uint64_t{average_bps} / safe_fps;
    uint32_t vbv_bits = saturate_u32(per_frame_bits * STREAM_VBV_FRAMES);
    if (vbv_bits < STREAM_VBV_MIN_BITS) vbv_bits = STREAM_VBV_MIN_BITS;
    const uint32_t max_frame_bits = saturate_u32(per_frame_bits * STREAM_MAX_FRAME_FRAMES);
    return {average_bps, peak_bps_for(average_bps), vbv_bits, vbv_bits,
            max_frame_bits < vbv_bits ? vbv_bits : max_frame_bits};
}

}
