#include "nvidia/nvdec_sequence_policy.h"

#include <cstdlib>
#include <iostream>

using parties::encdec::nvidia::detail::NvdecSequenceFormat;
using parties::encdec::nvidia::detail::NvdecSequenceState;
using parties::encdec::nvidia::detail::can_reuse_decoder;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    const NvdecSequenceState state{
        true, 3840, 2160, 3840, 2176, 0, 0, 8, 32};
    const NvdecSequenceFormat repeated{
        3840, 2160, 3840, 2176, 0, 0, 8, 30};

    check(can_reuse_decoder(state, repeated),
          "a 30-surface AV1 sequence must reuse a 32-surface decoder");

    auto too_many_surfaces = repeated;
    too_many_surfaces.minimum_decode_surfaces = 33;
    check(!can_reuse_decoder(state, too_many_surfaces),
          "a decoder with too few surfaces must be recreated");

    auto resized = repeated;
    resized.width = 2560;
    check(!can_reuse_decoder(state, resized),
          "a visible resolution change must recreate the decoder");

    auto recropped = repeated;
    recropped.crop_y = 8;
    check(!can_reuse_decoder(state, recropped),
          "a crop change must recreate the decoder");

    auto deeper = repeated;
    deeper.bit_depth = 10;
    check(!can_reuse_decoder(state, deeper),
          "a bit-depth change must recreate the decoder");

    std::cout << "NVDEC sequence reuse policy passed\n";
    return 0;
}
