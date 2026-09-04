#include <encdec/rate_control.h>

#include <cstdint>
#include <iostream>
#include <limits>

using parties::encdec::make_stream_vbr_rate_control;
using parties::encdec::STREAM_MAX_FRAME_FRAMES;
using parties::encdec::STREAM_VBV_FRAMES;
using parties::encdec::STREAM_VBV_MIN_BITS;

int main() {
    // 2 Mbps at 30 fps: one frame interval is 66'666 bits.
    //   peak            = 1.5 x 2 Mbps            = 3'000'000
    //   vbv_buffer_size = 4 frames  = 4 x 66'666  =   266'664
    //   vbv_initial     = vbv
    //   max_frame_bits  = 6 frames  = 6 x 66'666  =   399'996
    const auto normal = make_stream_vbr_rate_control(2'000'000, 30);
    constexpr uint32_t per_frame = 2'000'000 / 30;
    if (normal.average_bitrate != 2'000'000 ||
        normal.peak_bitrate != 3'000'000 ||
        normal.vbv_buffer_size != per_frame * STREAM_VBV_FRAMES ||
        normal.vbv_buffer_size != 266'664 ||
        normal.vbv_initial_delay != normal.vbv_buffer_size ||
        normal.max_frame_bits != per_frame * STREAM_MAX_FRAME_FRAMES ||
        normal.max_frame_bits != 399'996) {
        std::cerr << "unexpected stream VBR policy for 2 Mbps @ 30 fps\n";
        return 1;
    }

    // 8 Mbps at 60 fps: one frame interval is 133'333 bits.
    const auto fast = make_stream_vbr_rate_control(8'000'000, 60);
    if (fast.peak_bitrate != 12'000'000 ||
        fast.vbv_buffer_size != 533'332 ||
        fast.vbv_initial_delay != 533'332 ||
        fast.max_frame_bits != 799'998) {
        std::cerr << "unexpected stream VBR policy for 8 Mbps @ 60 fps\n";
        return 1;
    }

    // 2 Mbps at 60 fps: four frame intervals (133'332 bits) are below the
    // 250 kbit floor, so the floor wins and the per-frame cap follows it.
    const auto fast_low = make_stream_vbr_rate_control(2'000'000, 60);
    if (fast_low.peak_bitrate != 3'000'000 ||
        fast_low.vbv_buffer_size != STREAM_VBV_MIN_BITS ||
        fast_low.max_frame_bits != STREAM_VBV_MIN_BITS) {
        std::cerr << "VBV floor not applied for 2 Mbps @ 60 fps\n";
        return 1;
    }

    // Low bitrate: the reservoir floor (250 kbit) applies, and the per-frame cap
    // is never below the reservoir.
    const auto low = make_stream_vbr_rate_control(200'000, 30);
    if (low.average_bitrate != 200'000 ||
        low.peak_bitrate != 300'000 ||
        low.vbv_buffer_size != STREAM_VBV_MIN_BITS ||
        low.vbv_initial_delay != STREAM_VBV_MIN_BITS ||
        low.max_frame_bits != STREAM_VBV_MIN_BITS) {
        std::cerr << "VBV floor not applied at a low bitrate\n";
        return 1;
    }

    // Saturation: the peak must not overflow uint32.
    const auto saturated = make_stream_vbr_rate_control(
        std::numeric_limits<uint32_t>::max(), 30);
    if (saturated.average_bitrate != std::numeric_limits<uint32_t>::max() ||
        saturated.peak_bitrate != std::numeric_limits<uint32_t>::max() ||
        saturated.vbv_buffer_size != (std::numeric_limits<uint32_t>::max() / 30) * STREAM_VBV_FRAMES ||
        saturated.max_frame_bits != (std::numeric_limits<uint32_t>::max() / 30) * STREAM_MAX_FRAME_FRAMES) {
        std::cerr << "VBR rate control overflowed at UINT32_MAX\n";
        return 1;
    }
    // At 1 fps every frame interval is the whole second: the caps saturate too.
    const auto saturated_slow = make_stream_vbr_rate_control(
        std::numeric_limits<uint32_t>::max(), 1);
    if (saturated_slow.vbv_buffer_size != std::numeric_limits<uint32_t>::max() ||
        saturated_slow.max_frame_bits != std::numeric_limits<uint32_t>::max()) {
        std::cerr << "VBV / max frame size overflowed at UINT32_MAX @ 1 fps\n";
        return 1;
    }

    // fps 0 falls back to 30 fps.
    const auto fallback = make_stream_vbr_rate_control(2'000'000, 0);
    if (fallback.average_bitrate != normal.average_bitrate ||
        fallback.peak_bitrate != normal.peak_bitrate ||
        fallback.vbv_buffer_size != normal.vbv_buffer_size ||
        fallback.vbv_initial_delay != normal.vbv_initial_delay ||
        fallback.max_frame_bits != normal.max_frame_bits) {
        std::cerr << "fps 0 did not fall back to the 30 fps policy\n";
        return 1;
    }

    std::cout << "Stream VBR rate control policy passed\n";
    return 0;
}
