#include <client/video_decode_gate.h>
#include <client/video_decode_backlog.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <queue>

using parties::client::VideoDecodeDecision;
using parties::client::VideoDecodeGate;
using parties::client::VideoDecodeStaleFlushTracker;

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

    // Age rule: a warm decoder whose oldest queued frame has waited the full
    // budget (and has no keyframe to jump to) must flush and re-request one.
    constexpr int64_t age = parties::client::kVideoDecodeBacklogMaxAgeUs;
    const int64_t oldest = 1'000'000;
    if (!parties::client::should_resync_stale_decode_backlog(
            oldest, oldest + age, false, true)) {
        std::cerr << "stale backlog did not request a keyframe\n";
        return 1;
    }
    if (parties::client::should_resync_stale_decode_backlog(
            oldest, oldest + age - 1, false, true)) {
        std::cerr << "backlog younger than the age budget was discarded\n";
        return 1;
    }
    if (parties::client::should_resync_stale_decode_backlog(
            oldest, oldest + age * 2, true, true)) {
        std::cerr << "stale backlog with a queued keyframe was discarded instead of trimmed\n";
        return 1;
    }
    if (parties::client::should_resync_stale_decode_backlog(
            oldest, oldest + age * 2, false, false)) {
        std::cerr << "cold decoder backlog was discarded by the age rule\n";
        return 1;
    }

    // Anti-flap: a decoder that is persistently slower than the stream may
    // request a keyframe for the first two consecutive stale flushes only.
    if (!parties::client::should_request_keyframe_after_stale_flush(1) ||
        !parties::client::should_request_keyframe_after_stale_flush(2) ||
        parties::client::should_request_keyframe_after_stale_flush(3)) {
        std::cerr << "stale flush keyframe request limit is not two consecutive flushes\n";
        return 1;
    }

    // The recovery keyframe that necessarily follows every flush (the gate
    // drops deltas until one arrives) must not end the run, or the counter
    // could never pass one and the ~1 Hz flush/PLI cycle would continue.
    constexpr int64_t recovery = parties::client::kVideoDecodeStaleFlushRecoveryUs;
    VideoDecodeStaleFlushTracker tracker;
    if (tracker.suppressed()) {
        std::cerr << "fresh stream suppresses keyframe requests\n";
        return 1;
    }
    if (!tracker.on_stale_flush()) {
        std::cerr << "first stale flush did not request a keyframe\n";
        return 1;
    }
    tracker.on_keyframe_decoded(200'000, true);
    if (tracker.stale_flushes_in_a_row != 1) {
        std::cerr << "recovery keyframe ended the stale flush run\n";
        return 1;
    }
    if (!tracker.on_stale_flush()) {
        std::cerr << "second stale flush did not request a keyframe\n";
        return 1;
    }
    tracker.on_keyframe_decoded(1'700'000, true);
    if (tracker.on_stale_flush() || !tracker.suppressed() ||
        tracker.stale_flushes_in_a_row != 3) {
        std::cerr << "third consecutive stale flush still requested a keyframe\n";
        return 1;
    }
    // The sharer's periodic keyframe arrives while the gate waits for one: it
    // resumes decoding but proves nothing about the decoder's speed.
    tracker.on_keyframe_decoded(4'900'000, true);
    if (!tracker.suppressed()) {
        std::cerr << "periodic recovery keyframe lifted the suppression\n";
        return 1;
    }
    // An in-sequence keyframe landing before the recovery window has elapsed
    // (e.g. another viewer's PLI shortly after we resumed) does not count.
    tracker.on_keyframe_decoded(4'900'000 + recovery - 1, false);
    if (!tracker.suppressed()) {
        std::cerr << "in-sequence keyframe before the recovery window lifted the suppression\n";
        return 1;
    }
    // One clean recovery window of decoding ends the run: requests resume.
    tracker.on_keyframe_decoded(4'900'000 + recovery, false);
    if (tracker.suppressed() || tracker.stale_flushes_in_a_row != 0) {
        std::cerr << "keyframe decoded normally after a clean interval did not reset the counter\n";
        return 1;
    }
    if (!tracker.on_stale_flush() || tracker.stale_flushes_in_a_row != 1) {
        std::cerr << "stale flush after a recovery did not request a keyframe\n";
        return 1;
    }
    // A flush restarts the recovery clock: the next in-sequence keyframe must
    // again wait for a full window after decoding resumes.
    tracker.on_keyframe_decoded(20'000'000, true);
    tracker.on_keyframe_decoded(20'000'000 + recovery - 1, false);
    if (tracker.stale_flushes_in_a_row != 1) {
        std::cerr << "recovery clock was not restarted by the flush\n";
        return 1;
    }

    std::cout << "Video decode reference gate and backlog policy passed\n";
    return 0;
}
