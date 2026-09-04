#pragma once

#include <cstdint>
#include <limits>

namespace parties::encdec {

// All values in bits (per second for the bitrates).
struct VbrRateControl {
    uint32_t average_bitrate = 0;
    uint32_t peak_bitrate = 0;
    uint32_t vbv_buffer_size = 0;     // HRD/VBV reservoir
    uint32_t vbv_initial_delay = 0;   // initial fullness (NVENC only; AMF uses a 0..64 scale)
    uint32_t max_frame_bits = 0;      // per-frame cap for backends that have one (AMF MAX_AU_SIZE /
                                      // AV1 MAX_COMPRESSED_FRAME_SIZE); 0 = no cap
};

// Low-latency screen-share policy.
//
// The UI bitrate is an average target. The old policy (peak = 2x average, VBV
// = 2x average bits, i.e. a two-second reservoir) let a single keyframe be
// several hundred KB: on a link close to the average that is a ~1 s burst that
// every viewer experiences as a stall followed by catch-up. For a low-latency
// stream the reservoir must be a small number of FRAME intervals, so it is
// expressed in frames:
//   peak            = 1.5 x average
//   vbv_buffer_size = max(4 frame intervals at average, 250 kbit)
//   vbv_initial_delay = vbv_buffer_size
//   max_frame_bits  = 6 frame intervals at average (~200 ms of link time at
//                     30 fps), used only by backends with a per-frame cap.
// Which fields each backend consumes:
//   NVENC        average, peak, vbv_buffer_size, vbv_initial_delay
//   AMF          average, peak, vbv_buffer_size, max_frame_bits
//   MFT          average, peak, vbv_buffer_size (CODECAPI_AVEncCommonBufferSize)
//   VideoToolbox average + DataRateLimits window derived from peak
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

} // namespace parties::encdec
