// Offline, deterministic tests for parties::VideoFrameReorderBuffer (viewer-side
// reordering of frames that complete out of order on per-frame QUIC streams).

#include <parties/video_frame_reorder.h>

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <vector>

using parties::VideoFrameHeader;
using parties::VideoFrameReorderBuffer;
using parties::VideoFrameReorderConfig;
using parties::VIDEO_FLAG_KEYFRAME;
using parties::VIDEO_FRAME_HEADER_SIZE;

namespace {

constexpr int64_t T0 = 5'000'000;
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

// Test harness: records delivered frame_seqs and reported gaps.
struct Sink {
    std::vector<uint32_t> delivered;
    std::vector<uint32_t> lost;
    size_t delivered_bytes = 0;

    VideoFrameReorderBuffer::DeliverFn deliver_fn() {
        return [this](const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame) {
            VideoFrameHeader parsed;
            if (!VideoFrameHeader::parse(frame.data(), frame.size(), parsed) ||
                parsed.frame_seq != hdr.frame_seq) {
                std::cerr << "delivered buffer does not match its header\n";
                delivered.push_back(std::numeric_limits<uint32_t>::max() - 1);
                return;
            }
            delivered.push_back(hdr.frame_seq);
            delivered_bytes += frame.size();
        };
    }
    VideoFrameReorderBuffer::LostFn lost_fn() {
        return [this](uint32_t seq) { lost.push_back(seq); };
    }
};

std::vector<uint8_t> make_frame(uint32_t seq, bool keyframe, size_t payload = 100) {
    VideoFrameHeader hdr;
    hdr.frame_seq = seq;
    hdr.timestamp = seq;
    hdr.flags = keyframe ? VIDEO_FLAG_KEYFRAME : 0;
    hdr.width = 1920;
    hdr.height = 1080;
    hdr.codec = 1;
    std::vector<uint8_t> buf(VIDEO_FRAME_HEADER_SIZE + payload, 0xAB);
    hdr.write(buf.data());
    return buf;
}

void feed(VideoFrameReorderBuffer& b, Sink& s, uint32_t seq, bool keyframe, int64_t now,
          size_t payload = 100) {
    auto frame = make_frame(seq, keyframe, payload);
    VideoFrameHeader hdr;
    VideoFrameHeader::parse(frame.data(), frame.size(), hdr);
    b.on_frame(hdr, std::move(frame), now, s.deliver_fn(), s.lost_fn());
}

bool same(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) {
    return a == std::vector<uint32_t>(b);
}

bool test_in_order() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 100, true, T0);
    feed(b, s, 101, false, T0 + 33 * MS);
    feed(b, s, 102, false, T0 + 66 * MS);
    if (!same(s.delivered, {100, 101, 102})) return fail("in-order: delivery order");
    if (!s.lost.empty()) return fail("in-order: spurious loss report");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("in-order: nothing should be held");
    if (!b.has_baseline() || b.last_delivered() != 102) return fail("in-order: baseline");
    if (s.delivered_bytes != 3 * (VIDEO_FRAME_HEADER_SIZE + 100)) return fail("in-order: delivered bytes");
    return true;
}

bool test_hold_then_fill() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 20 * MS);
    if (!same(s.delivered, {10})) return fail("hold: newer frames delivered over a gap");
    if (b.held_frames() != 2 || b.held_bytes() != 2 * (VIDEO_FRAME_HEADER_SIZE + 100))
        return fail("hold: held accounting");
    // The timer has not expired yet.
    b.poll(T0 + 100 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.delivered, {10}) || !s.lost.empty()) return fail("hold: poll gave up too early");
    feed(b, s, 11, false, T0 + 120 * MS);
    if (!same(s.delivered, {10, 11, 12, 13})) return fail("hold: fill did not drain");
    if (!s.lost.empty()) return fail("hold: fill reported a loss");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("hold: buffer not empty after drain");
    return true;
}

bool test_hold_then_timeout() {
    VideoFrameReorderConfig cfg;
    cfg.max_hold_ms = 150;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 40 * MS);
    b.poll(T0 + 159 * MS, s.deliver_fn(), s.lost_fn());   // 149 ms since 12 arrived
    if (!same(s.delivered, {10}) || !s.lost.empty()) return fail("timeout: gave up before max_hold_ms");
    b.poll(T0 + 160 * MS, s.deliver_fn(), s.lost_fn());   // 150 ms
    if (!same(s.lost, {11})) return fail("timeout: lost must be reported once with the first missing seq");
    if (!same(s.delivered, {10, 12, 13})) return fail("timeout: held frames not released");
    if (b.last_delivered() != 13 || b.held_frames() != 0) return fail("timeout: state after skip");
    b.poll(T0 + 500 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11})) return fail("timeout: loss reported twice");
    // The late frame 11 is now stale and ignored.
    feed(b, s, 11, false, T0 + 200 * MS);
    if (!same(s.delivered, {10, 12, 13})) return fail("timeout: stale frame delivered");
    // Filling a later gap partially must not restart the timer: 15 and 16
    // arrive, then 14 does not; the wait started when 15 arrived.
    feed(b, s, 16, false, T0 + 300 * MS);
    feed(b, s, 15, false, T0 + 400 * MS);
    b.poll(T0 + 449 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11})) return fail("timeout: timer restarted by a later-arriving older frame");
    b.poll(T0 + 450 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11, 14})) return fail("timeout: second gap not reported");
    if (!same(s.delivered, {10, 12, 13, 15, 16})) return fail("timeout: second gap not released");
    return true;
}

bool test_abort_resolves_gap() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    b.on_frame_aborted(11, T0 + 20 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11})) return fail("abort: missing frame not reported");
    if (!same(s.delivered, {10, 12})) return fail("abort: held frame not released");
    if (b.last_delivered() != 12) return fail("abort: baseline after release");
    // Abort of a frame nobody waits for is ignored.
    b.on_frame_aborted(5, T0 + 30 * MS, s.deliver_fn(), s.lost_fn());
    b.on_frame_aborted(20, T0 + 30 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11}) || !same(s.delivered, {10, 12})) return fail("abort: unrelated abort had an effect");
    // Abort of the next expected frame with nothing held: reported, baseline advances.
    b.on_frame_aborted(13, T0 + 40 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11, 13}) || b.last_delivered() != 13) return fail("abort: tail abort");
    feed(b, s, 14, false, T0 + 50 * MS);
    if (!same(s.delivered, {10, 12, 14})) return fail("abort: frame after aborted one not delivered");
    // Abort of a frame behind an open gap only removes it (tolerance path).
    feed(b, s, 16, false, T0 + 60 * MS);
    feed(b, s, 17, false, T0 + 60 * MS);
    b.on_frame_aborted(17, T0 + 70 * MS, s.deliver_fn(), s.lost_fn());
    if (b.held_frames() != 1 || b.held_bytes() != VIDEO_FRAME_HEADER_SIZE + 100)
        return fail("abort: held frame not removed");
    b.on_frame_aborted(15, T0 + 80 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.delivered, {10, 12, 14, 16}) || !same(s.lost, {11, 13, 15}))
        return fail("abort: second gap not resolved");
    return true;
}

bool test_keyframe_over_gap() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 20 * MS);
    feed(b, s, 21, false, T0 + 30 * MS);     // behind a second gap, newer than the keyframe
    feed(b, s, 20, true, T0 + 40 * MS);      // keyframe over the gap 11
    // The keyframe re-anchors the decoder by itself: no loss report (which
    // would only trigger a pointless PLI).
    if (!s.lost.empty()) return fail("keyframe: a gap closed by a keyframe must not be reported");
    if (!same(s.delivered, {10, 20, 21})) return fail("keyframe: not delivered immediately with its successors");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("keyframe: older held frames not discarded");
    // A late pre-keyframe frame is stale.
    feed(b, s, 11, false, T0 + 50 * MS);
    feed(b, s, 13, false, T0 + 50 * MS);
    if (!same(s.delivered, {10, 20, 21})) return fail("keyframe: stale frame delivered");
    if (b.last_delivered() != 21) return fail("keyframe: baseline");
    // A later gap that has to be resumed at a delta frame is still reported.
    feed(b, s, 23, false, T0 + 60 * MS);
    b.poll(T0 + 210 * MS, s.deliver_fn(), s.lost_fn());   // 150 ms after 23 arrived
    if (!same(s.lost, {22})) return fail("keyframe: later delta-frame gap not reported");
    if (!same(s.delivered, {10, 20, 21, 23})) return fail("keyframe: later gap not released");
    return true;
}

bool test_backward_jump_reset() {
    VideoFrameReorderConfig cfg;
    cfg.jump_reset_threshold = 256;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 5000, true, T0);
    feed(b, s, 5002, false, T0 + 10 * MS);   // held
    // A backward jump within the threshold is just an old frame.
    feed(b, s, 5000 - 200, true, T0 + 15 * MS);
    if (!same(s.delivered, {5000}) || b.held_frames() != 1 || b.last_delivered() != 5000)
        return fail("restart: small backward jump was treated as a reset");
    // Sharer restarted its counter: distance -5000 < -256 and 0 < 5000.
    feed(b, s, 0, true, T0 + 20 * MS);
    if (!same(s.delivered, {5000, 0})) return fail("restart: frame after counter reset not delivered");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("restart: stale held frames survived");
    if (!s.lost.empty()) return fail("restart: reported a loss");
    feed(b, s, 1, false, T0 + 30 * MS);
    if (!same(s.delivered, {5000, 0, 1}) || b.last_delivered() != 1) return fail("restart: continuation");
    // A restart does not need to be a keyframe to be recognised (the decode
    // gate handles the reference chain): 300 -> 0 is a restart too.
    VideoFrameReorderBuffer b2(cfg);
    Sink s2;
    feed(b2, s2, 300, true, T0);
    // ...while 300 -> 100 (distance -200) is merely stale.
    feed(b2, s2, 100, false, T0 + 5 * MS);
    if (!same(s2.delivered, {300}) || b2.last_delivered() != 300) return fail("restart: stale frame reset the baseline");
    feed(b2, s2, 0, false, T0 + 10 * MS);
    if (!same(s2.delivered, {300, 0}) || b2.last_delivered() != 0) return fail("restart: delta-frame restart");
    if (!s2.lost.empty()) return fail("restart: delta-frame restart reported a loss");
    return true;
}

bool test_u32_wrap() {
    VideoFrameReorderBuffer b;
    Sink s;
    const uint32_t max = std::numeric_limits<uint32_t>::max();
    feed(b, s, max - 1, true, T0);
    feed(b, s, 1, false, T0 + 10 * MS);       // held (gap: max, 0)
    feed(b, s, max, false, T0 + 20 * MS);
    if (!same(s.delivered, {max - 1, max})) return fail("wrap: pre-wrap frame");
    feed(b, s, 0, false, T0 + 30 * MS);
    if (!same(s.delivered, {max - 1, max, 0, 1})) return fail("wrap: 0xFFFFFFFF -> 0 not treated as consecutive");
    if (!s.lost.empty() || b.held_frames() != 0) return fail("wrap: state");
    // Frames from before the wrap are stale, not a counter reset.
    feed(b, s, max - 100, false, T0 + 40 * MS);
    if (!same(s.delivered, {max - 1, max, 0, 1})) return fail("wrap: pre-wrap frame delivered after wrap");
    if (b.last_delivered() != 1) return fail("wrap: baseline reset by a stale pre-wrap frame");
    // A stale frame far before the wrap (beyond jump_reset_threshold) is still stale.
    feed(b, s, max - 5000, false, T0 + 40 * MS);
    if (b.last_delivered() != 1 || !same(s.delivered, {max - 1, max, 0, 1}))
        return fail("wrap: far pre-wrap frame treated as a restart");
    feed(b, s, 2, false, T0 + 50 * MS);
    if (!same(s.delivered, {max - 1, max, 0, 1, 2})) return fail("wrap: continuation after wrap");
    return true;
}

bool test_duplicates_and_old() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 11, false, T0 + 10 * MS);
    feed(b, s, 11, false, T0 + 11 * MS);     // duplicate of the last delivered
    feed(b, s, 10, true, T0 + 12 * MS);      // old keyframe
    feed(b, s, 9, false, T0 + 12 * MS);      // older
    if (!same(s.delivered, {10, 11})) return fail("duplicates: old or duplicate frame delivered");
    feed(b, s, 13, false, T0 + 20 * MS);
    feed(b, s, 13, false, T0 + 21 * MS);     // duplicate of a held frame
    if (b.held_frames() != 1 || b.held_bytes() != VIDEO_FRAME_HEADER_SIZE + 100)
        return fail("duplicates: duplicate held frame stored twice");
    feed(b, s, 12, false, T0 + 30 * MS);
    if (!same(s.delivered, {10, 11, 12, 13})) return fail("duplicates: drain");
    if (!s.lost.empty()) return fail("duplicates: spurious loss");
    return true;
}

bool test_max_held_frames_overflow() {
    VideoFrameReorderConfig cfg;
    cfg.max_held_frames = 3;
    cfg.max_hold_ms = 10'000;   // never time out in this test
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 1 * MS);
    feed(b, s, 13, false, T0 + 2 * MS);
    feed(b, s, 14, false, T0 + 3 * MS);
    if (b.held_frames() != 3 || !same(s.delivered, {10})) return fail("overflow: below the limit");
    feed(b, s, 15, false, T0 + 4 * MS);
    if (!same(s.lost, {11})) return fail("overflow: gap not reported");
    if (!same(s.delivered, {10, 12, 13, 14, 15})) return fail("overflow: held frames not released");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("overflow: buffer not empty");
    // Two gaps: overflow releases only up to the second gap.
    feed(b, s, 17, false, T0 + 5 * MS);
    feed(b, s, 19, false, T0 + 6 * MS);
    feed(b, s, 20, false, T0 + 7 * MS);
    feed(b, s, 21, false, T0 + 8 * MS);
    if (!same(s.lost, {11, 16})) return fail("overflow: first of two gaps not reported");
    if (!same(s.delivered, {10, 12, 13, 14, 15, 17})) return fail("overflow: released past the second gap");
    if (b.held_frames() != 3) return fail("overflow: second gap frames not kept");
    if (b.held_bytes() != 3 * (VIDEO_FRAME_HEADER_SIZE + 100)) return fail("overflow: held bytes");
    return true;
}

bool test_max_bytes_overflow() {
    VideoFrameReorderConfig cfg;
    cfg.max_bytes = 1000;
    cfg.max_hold_ms = 10'000;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 1 * MS, 400);   // 414 bytes
    feed(b, s, 13, false, T0 + 2 * MS, 400);   // 828 bytes
    if (b.held_bytes() != 828) return fail("bytes: accounting");
    feed(b, s, 14, false, T0 + 3 * MS, 400);   // 1242 > 1000
    if (!same(s.lost, {11}) || !same(s.delivered, {10, 12, 13, 14}))
        return fail("bytes: overflow did not release the held frames");
    if (b.held_bytes() != 0) return fail("bytes: not zero after release");
    return true;
}

bool test_reset() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 1 * MS);
    b.reset();
    if (b.has_baseline() || b.held_frames() != 0 || b.held_bytes() != 0) return fail("reset: state");
    feed(b, s, 3, false, T0 + 2 * MS);   // delivered unconditionally
    if (!same(s.delivered, {10, 3}) || !s.lost.empty()) return fail("reset: first frame after reset");
    // The old held frame is now a "newer with gap" frame; it is held, not delivered.
    feed(b, s, 12, false, T0 + 3 * MS);
    if (!same(s.delivered, {10, 3}) || b.held_frames() != 1) return fail("reset: stale frame handling");
    return true;
}

} // namespace

int main() {
    if (!test_in_order()) return 1;
    if (!test_hold_then_fill()) return 1;
    if (!test_hold_then_timeout()) return 1;
    if (!test_abort_resolves_gap()) return 1;
    if (!test_keyframe_over_gap()) return 1;
    if (!test_backward_jump_reset()) return 1;
    if (!test_u32_wrap()) return 1;
    if (!test_duplicates_and_old()) return 1;
    if (!test_max_held_frames_overflow()) return 1;
    if (!test_max_bytes_overflow()) return 1;
    if (!test_reset()) return 1;

    std::cout << "Video frame reorder buffer policy passed\n";
    return 0;
}
