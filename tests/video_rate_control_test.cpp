#include <encdec/rate_control.h>

#include <cstdint>
#include <iostream>
#include <limits>

using parties::encdec::make_stream_vbr_rate_control;
using parties::encdec::peak_bps_for;
using parties::encdec::STREAM_MAX_FRAME_FRAMES;
using parties::encdec::STREAM_VBV_FRAMES;
using parties::encdec::STREAM_VBV_MIN_BITS;

int main() {
    const auto normal = make_stream_vbr_rate_control(2'000'000, 30);
    constexpr uint32_t per_frame = 2'000'000 / 30;
    if (normal.average_bps != 2'000'000 ||
        normal.peak_bps != 3'000'000 ||
        normal.vbv_buffer_bits != per_frame * STREAM_VBV_FRAMES ||
        normal.vbv_buffer_bits != 266'664 ||
        normal.vbv_initial_delay_bits != normal.vbv_buffer_bits ||
        normal.max_frame_bits != per_frame * STREAM_MAX_FRAME_FRAMES ||
        normal.max_frame_bits != 399'996) {
        std::cerr << "unexpected stream VBR policy for 2 Mbps @ 30 fps\n";
        return 1;
    }

    const auto fast = make_stream_vbr_rate_control(8'000'000, 60);
    if (fast.peak_bps != 12'000'000 ||
        fast.vbv_buffer_bits != 533'332 ||
        fast.vbv_initial_delay_bits != 533'332 ||
        fast.max_frame_bits != 799'998) {
        std::cerr << "unexpected stream VBR policy for 8 Mbps @ 60 fps\n";
        return 1;
    }

    const auto fast_low = make_stream_vbr_rate_control(2'000'000, 60);
    if (fast_low.peak_bps != 3'000'000 ||
        fast_low.vbv_buffer_bits != STREAM_VBV_MIN_BITS ||
        fast_low.max_frame_bits != STREAM_VBV_MIN_BITS) {
        std::cerr << "VBV floor not applied for 2 Mbps @ 60 fps\n";
        return 1;
    }

    const auto low = make_stream_vbr_rate_control(200'000, 30);
    if (low.average_bps != 200'000 ||
        low.peak_bps != 300'000 ||
        low.vbv_buffer_bits != STREAM_VBV_MIN_BITS ||
        low.vbv_initial_delay_bits != STREAM_VBV_MIN_BITS ||
        low.max_frame_bits != STREAM_VBV_MIN_BITS) {
        std::cerr << "VBV floor not applied at a low bitrate\n";
        return 1;
    }

    const auto saturated = make_stream_vbr_rate_control(
        std::numeric_limits<uint32_t>::max(), 30);
    if (saturated.average_bps != std::numeric_limits<uint32_t>::max() ||
        saturated.peak_bps != std::numeric_limits<uint32_t>::max() ||
        saturated.vbv_buffer_bits != (std::numeric_limits<uint32_t>::max() / 30) * STREAM_VBV_FRAMES ||
        saturated.max_frame_bits != (std::numeric_limits<uint32_t>::max() / 30) * STREAM_MAX_FRAME_FRAMES) {
        std::cerr << "VBR rate control overflowed at UINT32_MAX\n";
        return 1;
    }
    const auto saturated_slow = make_stream_vbr_rate_control(
        std::numeric_limits<uint32_t>::max(), 1);
    if (saturated_slow.vbv_buffer_bits != std::numeric_limits<uint32_t>::max() ||
        saturated_slow.max_frame_bits != std::numeric_limits<uint32_t>::max()) {
        std::cerr << "VBV / max frame size overflowed at UINT32_MAX @ 1 fps\n";
        return 1;
    }

    const auto fallback = make_stream_vbr_rate_control(2'000'000, 0);
    if (fallback.average_bps != normal.average_bps ||
        fallback.peak_bps != normal.peak_bps ||
        fallback.vbv_buffer_bits != normal.vbv_buffer_bits ||
        fallback.vbv_initial_delay_bits != normal.vbv_initial_delay_bits ||
        fallback.max_frame_bits != normal.max_frame_bits) {
        std::cerr << "fps 0 did not fall back to the 30 fps policy\n";
        return 1;
    }

    if (peak_bps_for(2'000'000) != 3'000'000 ||
        peak_bps_for(1) != 1 ||
        peak_bps_for(std::numeric_limits<uint32_t>::max()) != std::numeric_limits<uint32_t>::max()) {
        std::cerr << "peak_bps_for is not a saturating 1.5x of the average\n";
        return 1;
    }

    std::cout << "Stream VBR rate control policy passed\n";
    return 0;
}
