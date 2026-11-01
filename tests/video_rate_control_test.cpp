#include <encdec/rate_control.h>

#include <cstdint>
#include <iostream>
#include <limits>

using parties::encdec::make_stream_cbr_rate_control;
using parties::encdec::StreamRateControl;
using parties::encdec::STREAM_VBV_FRAMES;
using parties::encdec::STREAM_VBV_MIN_BITS;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

// CBR invariants that hold for every input: peak == average, the VBV starts
// full, and the per-frame cap equals the VBV.
bool is_cbr(const StreamRateControl& rc, uint32_t bitrate) {
    return rc.average_bitrate == bitrate &&
           rc.peak_bitrate == bitrate &&
           rc.vbv_initial_delay == rc.vbv_buffer_size &&
           rc.max_frame_bits == rc.vbv_buffer_size;
}

// The policy is usable in constant expressions.
static_assert(make_stream_cbr_rate_control(2'000'000, 30).vbv_buffer_size == 133'332);
static_assert(make_stream_cbr_rate_control(2'000'000, 30).peak_bitrate == 2'000'000);

} // namespace

int main() {
    // Two frame intervals at the target rate: bitrate / fps * 2.
    struct Case { uint32_t bitrate; uint32_t fps; uint32_t vbv; const char* name; };
    const Case cases[] = {
        { 2'000'000, 30, 133'332, "2 Mbps @ 30 fps"},   // 66'666 bits per frame
        { 2'000'000, 60,  66'666, "2 Mbps @ 60 fps"},   // 33'333 bits per frame
        { 6'000'000, 30, 400'000, "6 Mbps @ 30 fps"},
        { 8'000'000, 60, 266'666, "8 Mbps @ 60 fps"},
        {20'000'000, 60, 666'666, "20 Mbps @ 60 fps"},
        {20'000'000, 120, 333'332, "20 Mbps @ 120 fps"},
        {   750'000, 30,  50'000, "750 kbps @ 30 fps (exactly at the floor)"},
    };
    for (const auto& c : cases) {
        const auto rc = make_stream_cbr_rate_control(c.bitrate, c.fps);
        expect(is_cbr(rc, c.bitrate), c.name);
        expect(rc.vbv_buffer_size == c.vbv, c.name);
        expect(rc.vbv_buffer_size == (c.bitrate / c.fps) * STREAM_VBV_FRAMES ||
                   rc.vbv_buffer_size == STREAM_VBV_MIN_BITS,
               c.name);
    }

    // Floor: two frame intervals below 50 kbit are raised to the floor.
    const auto low = make_stream_cbr_rate_control(200'000, 30);       // 13'332 bits
    expect(is_cbr(low, 200'000), "200 kbps @ 30 fps is CBR");
    expect(low.vbv_buffer_size == STREAM_VBV_MIN_BITS, "VBV floor at 200 kbps @ 30 fps");
    const auto low_fast = make_stream_cbr_rate_control(1'000'000, 60); // 33'332 bits
    expect(is_cbr(low_fast, 1'000'000), "1 Mbps @ 60 fps is CBR");
    expect(low_fast.vbv_buffer_size == STREAM_VBV_MIN_BITS, "VBV floor at 1 Mbps @ 60 fps");
    const auto zero = make_stream_cbr_rate_control(0, 30);
    expect(zero.average_bitrate == 0 && zero.peak_bitrate == 0 &&
               zero.vbv_buffer_size == STREAM_VBV_MIN_BITS,
           "bitrate 0 keeps the VBV floor");
    // Just above the floor the VBV tracks the frame interval again.
    const auto above_floor = make_stream_cbr_rate_control(780'000, 30); // 52'000 bits
    expect(above_floor.vbv_buffer_size == 52'000, "VBV above the floor follows 2 frames");

    // A keyframe can never take more than two frame intervals of link time
    // (above the floor): max_frame_bits / bitrate <= 2 / fps.
    for (uint32_t fps : {15u, 24u, 30u, 60u, 120u, 144u}) {
        for (uint32_t bitrate = 1'000'000; bitrate <= 20'000'000; bitrate += 500'000) {
            const auto rc = make_stream_cbr_rate_control(bitrate, fps);
            expect(is_cbr(rc, bitrate), "sweep: CBR invariants");
            if (rc.vbv_buffer_size > STREAM_VBV_MIN_BITS) {
                expect(static_cast<uint64_t>(rc.max_frame_bits) * fps <=
                           static_cast<uint64_t>(bitrate) * STREAM_VBV_FRAMES,
                       "sweep: max frame within two frame intervals");
            }
            expect(rc.vbv_buffer_size >= STREAM_VBV_MIN_BITS, "sweep: VBV floor");
        }
    }

    // Saturation: UINT32_MAX at 30 fps fits; at 1 fps two frame intervals
    // overflow uint32 and must saturate instead of wrapping.
    constexpr uint32_t max_u32 = std::numeric_limits<uint32_t>::max();
    const auto saturated = make_stream_cbr_rate_control(max_u32, 30);
    expect(is_cbr(saturated, max_u32), "UINT32_MAX @ 30 fps is CBR");
    expect(saturated.vbv_buffer_size == (max_u32 / 30) * STREAM_VBV_FRAMES,
           "UINT32_MAX @ 30 fps VBV");
    const auto saturated_slow = make_stream_cbr_rate_control(max_u32, 1);
    expect(is_cbr(saturated_slow, max_u32), "UINT32_MAX @ 1 fps is CBR");
    expect(saturated_slow.vbv_buffer_size == max_u32, "UINT32_MAX @ 1 fps VBV saturates");
    expect(saturated_slow.max_frame_bits == max_u32, "UINT32_MAX @ 1 fps frame cap saturates");

    // fps 0 falls back to 30 fps.
    const auto fallback = make_stream_cbr_rate_control(2'000'000, 0);
    const auto normal = make_stream_cbr_rate_control(2'000'000, 30);
    expect(fallback.average_bitrate == normal.average_bitrate &&
               fallback.peak_bitrate == normal.peak_bitrate &&
               fallback.vbv_buffer_size == normal.vbv_buffer_size &&
               fallback.vbv_initial_delay == normal.vbv_initial_delay &&
               fallback.max_frame_bits == normal.max_frame_bits,
           "fps 0 falls back to the 30 fps policy");

    if (failures != 0) {
        std::cerr << failures << " stream CBR rate control check(s) failed\n";
        return 1;
    }
    std::cout << "Stream CBR rate control policy passed\n";
    return 0;
}
