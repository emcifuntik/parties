#pragma once

#include <cstdint>
#include <limits>

namespace parties::encdec {

struct VbrRateControl {
    uint32_t average_bitrate = 0;
    uint32_t peak_bitrate = 0;
    uint32_t vbv_buffer_size = 0;
    uint32_t vbv_initial_delay = 0;
};

// The UI bitrate is an average target. A one-second VBV reservoir at twice the
// average lets scene changes and keyframes spend extra bits without turning an
// otherwise low-latency stream into an unconstrained bandwidth source.
constexpr VbrRateControl make_stream_vbr_rate_control(uint32_t average_bitrate) {
    const uint64_t doubled = static_cast<uint64_t>(average_bitrate) * 2u;
    const uint32_t peak = doubled > (std::numeric_limits<uint32_t>::max)()
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(doubled);
    return {average_bitrate, peak, peak, peak};
}

} // namespace parties::encdec
