#pragma once

#include <cstdint>
#include <limits>

namespace parties::encdec {

// All values in bits (per second for the bitrates).
struct StreamRateControl {
    uint32_t average_bitrate = 0;
    uint32_t peak_bitrate = 0;        // == average_bitrate (CBR)
    uint32_t vbv_buffer_size = 0;     // HRD/VBV reservoir
    uint32_t vbv_initial_delay = 0;   // initial fullness (NVENC only; AMF uses a 0..64 scale)
    uint32_t max_frame_bits = 0;      // per-frame cap for backends that have one (AMF MAX_AU_SIZE /
                                      // AV1 MAX_COMPRESSED_FRAME_SIZE); 0 = no cap
};

// Low-latency screen-share policy: CBR with a VBV of two frame intervals.
//
// The UI / send-controller bitrate is the rate the link is expected to carry,
// so the encoder must never emit more than the link drains in a couple of
// frame intervals. Any reservoir larger than that turns into queueing delay:
// the previous peak-constrained VBR policy (peak 1.5x, VBV up to 250 kbit and
// per-frame caps of 6 frame intervals) let a keyframe take ~200 ms of link
// time, which viewers saw as a stall followed by a catch-up burst. With
//   average = peak   = bitrate
//   vbv_buffer_size  = max(2 frame intervals at bitrate, 50 kbit)
//   vbv_initial_delay = vbv_buffer_size
//   max_frame_bits   = vbv_buffer_size
// a frame, keyframes included, never needs more than ~2 frame intervals of
// link time. The first keyframe after a join or PLI is therefore coarse; the
// following P frames refine it within a fraction of a second, which is far
// less visible than a freeze. For the same reason the encoders do not emit
// periodic keyframes (VIDEO_KEYFRAME_INTERVAL_MS is a long safety net): on a
// static screen each one would be a visible pulse. The 50 kbit floor only matters at very low
// bitrates / high frame rates, where two frame intervals would leave too
// little room to code a keyframe at all.
//
// Filler data must stay disabled: CBR is used for its strict VBV bound, not to
// pad the stream up to the target rate.
//
// Which fields each backend consumes:
//   NVENC        average (= max bitrate), vbv_buffer_size, vbv_initial_delay
//   AMF          average (= peak), vbv_buffer_size, max_frame_bits
//   MFT          average, vbv_buffer_size (CODECAPI_AVEncCommonBufferSize, bytes)
//   OpenH264     average (= max bitrate); it has no VBV, frame skipping bounds it
//   VideoToolbox average + DataRateLimits windows derived from the VBV
constexpr uint32_t STREAM_VBV_MIN_BITS = 50'000;
constexpr uint32_t STREAM_VBV_FRAMES   = 2;

constexpr uint32_t saturate_u32(uint64_t v) {
    return v > (std::numeric_limits<uint32_t>::max)()
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(v);
}

constexpr StreamRateControl make_stream_cbr_rate_control(uint32_t bitrate, uint32_t fps) {
    const uint32_t safe_fps = fps == 0 ? 30u : fps;
    const uint64_t per_frame = static_cast<uint64_t>(bitrate) / safe_fps;
    uint32_t vbv = saturate_u32(per_frame * STREAM_VBV_FRAMES);
    if (vbv < STREAM_VBV_MIN_BITS) vbv = STREAM_VBV_MIN_BITS;
    return {bitrate, bitrate, vbv, vbv, vbv};
}

} // namespace parties::encdec
