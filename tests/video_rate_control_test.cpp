#include <encdec/rate_control.h>

#include <cstdint>
#include <iostream>
#include <limits>

int main() {
    const auto normal = parties::encdec::make_stream_vbr_rate_control(2'000'000);
    if (normal.average_bitrate != 2'000'000 ||
        normal.peak_bitrate != 4'000'000 ||
        normal.vbv_buffer_size != 4'000'000 ||
        normal.vbv_initial_delay != 4'000'000) {
        std::cerr << "unexpected stream VBR policy\n";
        return 1;
    }

    const auto saturated = parties::encdec::make_stream_vbr_rate_control(
        std::numeric_limits<uint32_t>::max());
    if (saturated.average_bitrate != std::numeric_limits<uint32_t>::max() ||
        saturated.peak_bitrate != std::numeric_limits<uint32_t>::max()) {
        std::cerr << "VBR peak bitrate overflowed\n";
        return 1;
    }

    std::cout << "Stream VBR average/peak policy passed\n";
    return 0;
}
