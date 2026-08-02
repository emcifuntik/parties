#include <client/video_decode_gate.h>
#include <client/video_decode_backlog.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <queue>

using parties::client::VideoDecodeDecision;
using parties::client::VideoDecodeGate;

namespace {
bool expect(VideoDecodeDecision actual, VideoDecodeDecision expected, const char* label) {
    if (actual == expected) return true;
    std::cerr << label << ": unexpected decision\n";
    return false;
}
} // namespace

int main() {
    VideoDecodeGate gate;
    if (!gate.awaiting_keyframe()) return 1;
    if (!expect(gate.on_frame(10, false), VideoDecodeDecision::DropUntilKeyframe,
                "initial delta")) return 1;
    if (!expect(gate.on_frame(10, true), VideoDecodeDecision::Accept,
                "initial keyframe")) return 1;
    if (!expect(gate.on_frame(11, false), VideoDecodeDecision::Accept,
                "contiguous delta")) return 1;
    if (!expect(gate.on_frame(13, false), VideoDecodeDecision::Discontinuity,
                "missing reference")) return 1;
    if (!gate.awaiting_keyframe()) return 1;
    if (!expect(gate.on_frame(14, false), VideoDecodeDecision::DropUntilKeyframe,
                "delta after gap")) return 1;
    if (!expect(gate.on_frame(20, true), VideoDecodeDecision::Accept,
                "recovery keyframe")) return 1;

    gate.require_keyframe();
    if (!expect(gate.on_frame(std::numeric_limits<uint32_t>::max(), true),
                VideoDecodeDecision::Accept, "wrap keyframe")) return 1;
    if (!expect(gate.on_frame(0, false), VideoDecodeDecision::Accept,
                "wrapped delta")) return 1;

    struct Work { int id; bool keyframe; };
    std::queue<Work> warmup_backlog;
    for (int i = 0; i < 17; ++i)
        warmup_backlog.push({i, false});
    const auto warmup_trim = parties::client::trim_to_latest_keyframe(
        warmup_backlog, [](const Work& work) { return work.keyframe; });
    if (warmup_trim.found_keyframe || warmup_trim.dropped != 0 ||
        warmup_backlog.size() != 17) {
        std::cerr << "warm-up backlog lost its contiguous reference chain\n";
        return 1;
    }
    if (parties::client::should_resync_decode_backlog(
            warmup_backlog.size(), warmup_trim.found_keyframe, true)) {
        std::cerr << "normal warm-up backlog incorrectly resets the decoder\n";
        return 1;
    }

    std::queue<Work> keyframe_backlog;
    for (int i = 0; i < 8; ++i)
        keyframe_backlog.push({i, i == 2 || i == 5});
    const auto keyframe_trim = parties::client::trim_to_latest_keyframe(
        keyframe_backlog, [](const Work& work) { return work.keyframe; });
    if (!keyframe_trim.found_keyframe || keyframe_trim.dropped != 5 ||
        keyframe_backlog.size() != 3 || keyframe_backlog.front().id != 5) {
        std::cerr << "backlog was not trimmed to the newest keyframe\n";
        return 1;
    }

    if (!parties::client::should_resync_decode_backlog(
            parties::client::kVideoDecodeBacklogHardLimitFrames + 1,
            false, true)) {
        std::cerr << "sustained backlog did not request a keyframe\n";
        return 1;
    }
    if (parties::client::should_resync_decode_backlog(
            parties::client::kVideoDecodeBacklogHardLimitFrames + 1,
            false, false)) {
        std::cerr << "cold decoder backlog was discarded before initialization\n";
        return 1;
    }

    std::cout << "Video decode reference gate and backlog policy passed\n";
    return 0;
}
