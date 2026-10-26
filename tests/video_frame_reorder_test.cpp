#include <parties/video_frame_reorder.h>

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

using parties::KeyframeRequest;
using parties::ParsedVideoFrame;
using parties::ReorderOutcome;
using parties::VideoCodecId;
using parties::VideoFrameHeader;
using parties::VideoFrameReorderBuffer;
using parties::VideoFrameReorderConfig;
using parties::VIDEO_FRAME_HEADER_SIZE;

namespace {

constexpr int64_t T0 = 5'000'000;
constexpr int64_t MS = 1000;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

struct Sink {
    std::vector<uint32_t> delivered;
    size_t keyframe_requests = 0;
    size_t delivered_bytes = 0;

    void take(KeyframeRequest request) {
        if (request == KeyframeRequest::Needed) ++keyframe_requests;
    }
    void take(ReorderOutcome&& out) {
        for (auto& frame : out.deliver) {
            const auto reparsed = VideoFrameHeader::parse(frame.wire());
            if (!reparsed || reparsed->frame_seq != frame.header().frame_seq) {
                std::cerr << "delivered buffer does not match its header\n";
                delivered.push_back(std::numeric_limits<uint32_t>::max() - 1);
                continue;
            }
            delivered.push_back(frame.header().frame_seq);
            delivered_bytes += frame.wire_size();
        }
        take(out.keyframe);
    }
};

ParsedVideoFrame make_frame(uint32_t seq, bool keyframe, size_t payload = 100) {
    VideoFrameHeader hdr;
    hdr.frame_seq = seq;
    hdr.keyframe = keyframe;
    hdr.width = 1920;
    hdr.height = 1080;
    hdr.codec = VideoCodecId::AV1;
    const std::vector<uint8_t> encoded(payload, 0xAB);
    return ParsedVideoFrame::assemble(hdr, encoded);
}

void feed(VideoFrameReorderBuffer& b, Sink& s, uint32_t seq, bool keyframe, int64_t now,
          size_t payload = 100) {
    s.take(b.on_frame(make_frame(seq, keyframe, payload), now));
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
    if (s.keyframe_requests != 0) return fail("in-order: spurious loss report");
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
    s.take(b.poll(T0 + 100 * MS));
    if (!same(s.delivered, {10}) || s.keyframe_requests != 0) return fail("hold: poll gave up too early");
    feed(b, s, 11, false, T0 + 120 * MS);
    if (!same(s.delivered, {10, 11, 12, 13})) return fail("hold: fill did not drain");
    if (s.keyframe_requests != 0) return fail("hold: fill reported a loss");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("hold: buffer not empty after drain");
    return true;
}

bool test_hold_then_timeout() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 40 * MS);
    s.take(b.poll(T0 + 159 * MS));
    if (!same(s.delivered, {10}) || s.keyframe_requests != 0) return fail("timeout: gave up too early");
    s.take(b.poll(T0 + 160 * MS));
    if (s.keyframe_requests != 1 || !b.wants_pli_retry()) return fail("timeout: recovery not requested");
    if (!same(s.delivered, {10}) || b.last_delivered() != 10)
        return fail("timeout: broken deltas advanced the decode baseline");
    feed(b, s, 11, false, T0 + 170 * MS);
    if (!same(s.delivered, {10, 11, 12, 13}) || b.wants_pli_retry())
        return fail("timeout: late delta did not resume the intact chain");
    feed(b, s, 11, true, T0 + 180 * MS);
    if (!same(s.delivered, {10, 11, 12, 13})) return fail("timeout: stale keyframe delivered");
    feed(b, s, 16, false, T0 + 300 * MS);
    feed(b, s, 15, false, T0 + 400 * MS);
    s.take(b.poll(T0 + 449 * MS));
    if (s.keyframe_requests != 1) return fail("timeout: second gap expired too early");
    s.take(b.poll(T0 + 450 * MS));
    s.take(b.poll(T0 + 800 * MS));
    if (s.keyframe_requests != 2 || !b.wants_pli_retry() || b.held_frames() != 2)
        return fail("timeout: loss repeated or held successors discarded");
    feed(b, s, 14, true, T0 + 810 * MS);
    feed(b, s, 15, false, T0 + 820 * MS);
    if (!same(s.delivered, {10, 11, 12, 13, 14, 15, 16}) || b.wants_pli_retry())
        return fail("timeout: recovery keyframe did not drain held successors");
    return true;
}

bool test_abort_resolves_gap() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    s.take(b.on_frame_aborted(11));
    if (s.keyframe_requests != 1 || !same(s.delivered, {10}) || !b.wants_pli_retry())
        return fail("abort: missing frame did not enter recovery");
    s.take(b.on_frame_aborted(11));
    s.take(b.on_frame_aborted(5));
    s.take(b.on_frame_aborted(20));
    if (s.keyframe_requests != 1) return fail("abort: repeated or unrelated abort reported loss");
    feed(b, s, 13, true, T0 + 40 * MS);
    s.take(b.on_frame_aborted(14));
    if (s.keyframe_requests != 2 || b.last_delivered() != 13 || !b.wants_pli_retry())
        return fail("abort: tail abort lost the last usable baseline");
    feed(b, s, 16, false, T0 + 60 * MS);
    feed(b, s, 17, false, T0 + 60 * MS);
    s.take(b.on_frame_aborted(17));
    if (b.held_frames() != 1 || b.held_bytes() != VIDEO_FRAME_HEADER_SIZE + 100)
        return fail("abort: held frame not removed");
    feed(b, s, 15, true, T0 + 80 * MS);
    if (!same(s.delivered, {10, 13, 15, 16}) || b.wants_pli_retry())
        return fail("abort: recovery keyframe did not resume delivery");
    return true;
}

bool test_keyframe_over_gap() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 20 * MS);
    feed(b, s, 21, false, T0 + 30 * MS);
    feed(b, s, 20, true, T0 + 40 * MS);
    if (s.keyframe_requests != 0) return fail("keyframe: a gap closed by a keyframe must not be reported");
    if (!same(s.delivered, {10, 20, 21})) return fail("keyframe: not delivered immediately with its successors");
    if (b.held_frames() != 0 || b.held_bytes() != 0) return fail("keyframe: older held frames not discarded");
    feed(b, s, 11, false, T0 + 50 * MS);
    feed(b, s, 13, false, T0 + 50 * MS);
    if (!same(s.delivered, {10, 20, 21})) return fail("keyframe: stale frame delivered");
    if (b.last_delivered() != 21) return fail("keyframe: baseline");
    feed(b, s, 23, false, T0 + 60 * MS);
    s.take(b.poll(T0 + 210 * MS));
    if (s.keyframe_requests != 1) return fail("keyframe: later delta-frame gap not reported");
    if (!same(s.delivered, {10, 20, 21}) || !b.wants_pli_retry())
        return fail("keyframe: later gap forwarded a broken delta");
    feed(b, s, 22, true, T0 + 220 * MS);
    if (!same(s.delivered, {10, 20, 21, 22, 23}))
        return fail("keyframe: late recovery keyframe did not drain its successor");
    return true;
}

bool test_late_frames_never_restart() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 5000, true, T0);
    feed(b, s, 5002, false, T0 + 10 * MS);
    feed(b, s, 0, true, T0 + 20 * MS);
    feed(b, s, 1, false, T0 + 21 * MS);
    feed(b, s, 4700, false, T0 + 22 * MS);
    if (!same(s.delivered, {5000}) || b.last_delivered() != 5000 || b.held_frames() != 1)
        return fail("late: old streams reset the active decode baseline");
    feed(b, s, 5001, false, T0 + 30 * MS);
    if (!same(s.delivered, {5000, 5001, 5002}) || s.keyframe_requests != 0)
        return fail("late: delayed frames disrupted the current chain");
    b.reset();
    feed(b, s, 0, true, T0 + 40 * MS);
    if (!same(s.delivered, {5000, 5001, 5002, 0}))
        return fail("late: explicit share restart did not reset the baseline");
    return true;
}

bool test_startup_reordering() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 12, false, T0);
    feed(b, s, 11, false, T0 + 5 * MS);
    feed(b, s, 12, false, T0 + 6 * MS);
    if (b.has_baseline() || !s.delivered.empty() || b.held_frames() != 2)
        return fail("startup: a delta committed the baseline or duplicate was retained");
    feed(b, s, 10, true, T0 + 20 * MS);
    if (!same(s.delivered, {10, 11, 12}) || s.keyframe_requests != 0 || b.held_bytes() != 0)
        return fail("startup: delayed first keyframe and successors not delivered in order");

    b.reset();
    s = Sink{};
    feed(b, s, 0, false, T0);
    feed(b, s, 1, false, T0 + MS);
    feed(b, s, UINT32_MAX, true, T0 + 2 * MS);
    if (!same(s.delivered, {UINT32_MAX, 0, 1}) || s.keyframe_requests != 0)
        return fail("startup: wrap-safe order");
    return true;
}

bool test_startup_bounds() {
    VideoFrameReorderConfig cfg;
    cfg.max_held_frames = 2;
    cfg.max_bytes = 250;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    for (uint32_t seq = 1; seq <= 20; ++seq)
        feed(b, s, seq, false, T0 + seq * MS);
    if (b.has_baseline() || b.held_frames() > 2 || b.held_bytes() > 250 || !s.delivered.empty())
        return fail("startup: unbounded delta accumulation");
    s.take(b.poll(T0 + 200 * MS));
    if (b.held_bytes() || b.held_frames() || s.keyframe_requests != 0)
        return fail("startup: timed-out deltas retained or reported as a decode gap");
    feed(b, s, 30, false, T0 + 210 * MS, 1000);
    if (b.held_bytes()) return fail("startup: oversize delta retained");
    feed(b, s, 31, true, T0 + 220 * MS);
    if (!same(s.delivered, {31})) return fail("startup: keyframe could not recover after overflow");
    return true;
}
bool test_u32_wrap() {
    VideoFrameReorderBuffer b;
    Sink s;
    const uint32_t max = std::numeric_limits<uint32_t>::max();
    feed(b, s, max - 1, true, T0);
    feed(b, s, 1, false, T0 + 10 * MS);
    feed(b, s, max, false, T0 + 20 * MS);
    if (!same(s.delivered, {max - 1, max})) return fail("wrap: pre-wrap frame");
    feed(b, s, 0, false, T0 + 30 * MS);
    if (!same(s.delivered, {max - 1, max, 0, 1})) return fail("wrap: 0xFFFFFFFF -> 0 not treated as consecutive");
    if (s.keyframe_requests != 0 || b.held_frames() != 0) return fail("wrap: state");
    feed(b, s, max - 100, false, T0 + 40 * MS);
    if (!same(s.delivered, {max - 1, max, 0, 1})) return fail("wrap: pre-wrap frame delivered after wrap");
    if (b.last_delivered() != 1) return fail("wrap: baseline reset by a stale pre-wrap frame");
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
    feed(b, s, 11, false, T0 + 11 * MS);
    feed(b, s, 10, true, T0 + 12 * MS);
    feed(b, s, 9, false, T0 + 12 * MS);
    if (!same(s.delivered, {10, 11})) return fail("duplicates: old or duplicate frame delivered");
    feed(b, s, 13, false, T0 + 20 * MS);
    feed(b, s, 13, false, T0 + 21 * MS);
    if (b.held_frames() != 1 || b.held_bytes() != VIDEO_FRAME_HEADER_SIZE + 100)
        return fail("duplicates: duplicate held frame stored twice");
    feed(b, s, 12, false, T0 + 30 * MS);
    if (!same(s.delivered, {10, 11, 12, 13})) return fail("duplicates: drain");
    if (s.keyframe_requests != 0) return fail("duplicates: spurious loss");
    return true;
}

bool test_max_held_frames_overflow() {
    VideoFrameReorderConfig cfg;
    cfg.max_held_frames = 3;
    cfg.max_hold_ms = 10'000;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    for (uint32_t seq = 12; seq <= 14; ++seq)
        feed(b, s, seq, false, T0 + seq * MS);
    if (b.held_frames() != 3 || s.keyframe_requests != 0) return fail("overflow: below limit");
    feed(b, s, 15, false, T0 + 15 * MS);
    if (s.keyframe_requests != 1 || !same(s.delivered, {10}) || !b.wants_pli_retry())
        return fail("overflow: broken deltas advanced the baseline");
    if (b.held_frames() != 3 || b.held_bytes() != 3 * (VIDEO_FRAME_HEADER_SIZE + 100))
        return fail("overflow: bounded recovery accounting");
    feed(b, s, 12, true, T0 + 20 * MS);
    if (!same(s.delivered, {10, 12, 13, 14, 15}) || b.held_frames() || b.wants_pli_retry())
        return fail("overflow: late keyframe and successors did not recover");
    return true;
}

bool test_max_bytes_overflow() {
    VideoFrameReorderConfig cfg;
    cfg.max_bytes = 1000;
    cfg.max_hold_ms = 10'000;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + MS, 400);
    feed(b, s, 13, false, T0 + 2 * MS, 400);
    if (b.held_bytes() != 828) return fail("bytes: accounting");
    feed(b, s, 14, false, T0 + 3 * MS, 400);
    if (s.keyframe_requests != 1 || !same(s.delivered, {10}) || b.held_bytes() != 828)
        return fail("bytes: overflow did not retain bounded recovery deltas");
    feed(b, s, 12, true, T0 + 4 * MS, 400);
    if (!same(s.delivered, {10, 12, 13, 14}) || b.held_bytes() != 0)
        return fail("bytes: recovery did not drain");
    return true;
}

bool test_reset() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 1 * MS);
    b.reset();
    if (b.has_baseline() || b.held_frames() != 0 || b.held_bytes() != 0) return fail("reset: state");
    feed(b, s, 3, true, T0 + 2 * MS);
    if (!same(s.delivered, {10, 3}) || s.keyframe_requests != 0) return fail("reset: first frame after reset");
    feed(b, s, 12, false, T0 + 3 * MS);
    if (!same(s.delivered, {10, 3}) || b.held_frames() != 1) return fail("reset: stale frame handling");
    return true;
}

bool test_delayed_recovery_keyframes() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 100, true, T0);
    for (uint32_t round = 0; round < 20; ++round) {
        const uint32_t key = 101 + round * 20;
        const int64_t now = T0 + round * 500 * MS;
        for (uint32_t delta = 1; delta <= 10; ++delta)
            feed(b, s, key + delta, false, now + delta * MS);
        feed(b, s, key, true, now + 200 * MS, 60'000);
        bool delivered_key = false;
        for (uint32_t seq : s.delivered) delivered_key |= seq == key;
        if (!delivered_key) return fail("recovery: delayed keyframe discarded after delta overflow");
        const VideoFrameReorderConfig bounds;
        if (b.held_frames() > bounds.max_held_frames || b.held_bytes() > bounds.max_bytes)
            return fail("recovery: buffer exceeded its bounds");
    }
    return true;
}

bool test_recovery_wrap_and_independent_viewers() {
    VideoFrameReorderBuffer slow, healthy;
    Sink stalled, playing;
    const uint32_t base = UINT32_MAX - 2;
    feed(slow, stalled, base, true, T0);
    feed(healthy, playing, base, true, T0);
    feed(healthy, playing, base + 1, false, T0 + MS);
    feed(healthy, playing, base + 2, false, T0 + 2 * MS);
    feed(slow, stalled, 0, false, T0 + 3 * MS);
    stalled.take(slow.poll(T0 + 160 * MS));
    if (!slow.wants_pli_retry() || healthy.wants_pli_retry())
        return fail("recovery: one viewer's loss affected another viewer");
    feed(slow, stalled, base - 100, true, T0 + 161 * MS);
    feed(slow, stalled, base, true, T0 + 162 * MS);
    if (!same(stalled.delivered, {base})) return fail("recovery: stale keyframe rolled back the stream");
    feed(slow, stalled, UINT32_MAX, true, T0 + 163 * MS);
    if (!same(stalled.delivered, {base, UINT32_MAX, 0}) || slow.wants_pli_retry())
        return fail("recovery: delayed keyframe across wrap did not drain");
    feed(slow, stalled, 1, false, T0 + 164 * MS);
    if (!same(stalled.delivered, {base, UINT32_MAX, 0, 1}))
        return fail("recovery: playback did not continue after recovery");
    slow.reset();
    if (!slow.wants_pli_retry() || slow.has_baseline()) return fail("recovery: reset state");
    return true;
}

bool test_gap_timer_at_time_zero() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, 0);
    feed(b, s, 12, false, 0);
    feed(b, s, 13, false, 50 * MS);
    feed(b, s, 15, false, 60 * MS);
    s.take(b.on_frame_aborted(15));
    s.take(b.poll(149 * MS));
    if (s.keyframe_requests != 0) return fail("t0=0: gap expired too early");
    s.take(b.poll(150 * MS));
    if (s.keyframe_requests != 1 || !same(s.delivered, {10}))
        return fail("t0=0: gap timer did not start at the first held frame");
    return true;
}

bool test_retransmitted_frame_after_timeout_resumes() {
    VideoFrameReorderBuffer b;
    Sink s;
    constexpr int64_t FRAME_US = 16'667;
    feed(b, s, 0, true, T0);
    for (uint32_t seq = 2; seq <= 15; ++seq) {
        const int64_t now = T0 + seq * FRAME_US;
        feed(b, s, seq, false, now);
        s.take(b.poll(now));
    }
    if (s.keyframe_requests != 1 || !same(s.delivered, {0}))
        return fail("retransmit: gap past the hold time did not hedge with one keyframe request");
    feed(b, s, 1, false, T0 + 250 * MS);
    if (!same(s.delivered, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}))
        return fail("retransmit: late frame did not resume the intact chain");
    if (b.wants_pli_retry() || b.held_frames() != 0)
        return fail("retransmit: recovered chain still waits for a keyframe");
    feed(b, s, 1, true, T0 + 400 * MS);
    feed(b, s, 16, false, T0 + 410 * MS);
    if (s.delivered.back() != 16 || s.delivered.size() != 17)
        return fail("retransmit: stale recovery keyframe disturbed playback");
    return true;
}

static_assert(!std::is_copy_constructible_v<VideoFrameReorderBuffer>);
static_assert(std::is_nothrow_move_constructible_v<VideoFrameReorderBuffer>);

}

int main() {
    if (!test_delayed_recovery_keyframes()) return 1;
    if (!test_recovery_wrap_and_independent_viewers()) return 1;
    if (!test_in_order()) return 1;
    if (!test_hold_then_fill()) return 1;
    if (!test_hold_then_timeout()) return 1;
    if (!test_abort_resolves_gap()) return 1;
    if (!test_keyframe_over_gap()) return 1;
    if (!test_late_frames_never_restart()) return 1;
    if (!test_startup_reordering()) return 1;
    if (!test_startup_bounds()) return 1;
    if (!test_u32_wrap()) return 1;
    if (!test_duplicates_and_old()) return 1;
    if (!test_max_held_frames_overflow()) return 1;
    if (!test_max_bytes_overflow()) return 1;
    if (!test_reset()) return 1;
    if (!test_gap_timer_at_time_zero()) return 1;
    if (!test_retransmitted_frame_after_timeout_resumes()) return 1;

    std::cout << "Video frame reorder buffer policy passed\n";
    return 0;
}
