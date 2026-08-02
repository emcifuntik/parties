#pragma once

#include <cstdint>

namespace parties::encdec::nvidia::detail {

struct NvdecSequenceState {
    bool decoder_active = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t bit_depth = 0;
    uint32_t decode_surfaces = 0;
};

struct NvdecSequenceFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t bit_depth = 0;
    uint32_t minimum_decode_surfaces = 0;
};

constexpr bool can_reuse_decoder(const NvdecSequenceState& state,
                                 const NvdecSequenceFormat& format) {
    return state.decoder_active &&
        state.width == format.width && state.height == format.height &&
        state.coded_width == format.coded_width &&
        state.coded_height == format.coded_height &&
        state.crop_x == format.crop_x && state.crop_y == format.crop_y &&
        state.bit_depth == format.bit_depth &&
        state.decode_surfaces >= format.minimum_decode_surfaces;
}

} // namespace parties::encdec::nvidia::detail
