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
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    feed(b, s, 13, false, T0 + 40 * MS);
    b.poll(T0 + 159 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.delivered, {10}) || !s.lost.empty()) return fail("timeout: gave up too early");
    b.poll(T0 + 160 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11}) || !b.needs_keyframe()) return fail("timeout: recovery not requested");
    if (!same(s.delivered, {10}) || b.last_delivered() != 10)
        return fail("timeout: broken deltas advanced the decode baseline");
    // A late delta cannot repair a reference chain already declared lost.
    feed(b, s, 11, false, T0 + 170 * MS);
    if (!same(s.delivered, {10})) return fail("timeout: late delta resumed decoding");
    feed(b, s, 11, true, T0 + 180 * MS);
    if (!same(s.delivered, {10, 11, 12, 13}) || b.needs_keyframe())
        return fail("timeout: late keyframe did not recover buffered successors");
    // Filling a later gap partially must not restart its timer.
    feed(b, s, 16, false, T0 + 300 * MS);
    feed(b, s, 15, false, T0 + 400 * MS);
    b.poll(T0 + 449 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11})) return fail("timeout: second gap expired too early");
    b.poll(T0 + 450 * MS, s.deliver_fn(), s.lost_fn());
    b.poll(T0 + 800 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11, 14}) || !b.needs_keyframe() || b.held_bytes() != 0)
        return fail("timeout: loss repeated or expired recovery deltas retained");
    feed(b, s, 14, true, T0 + 810 * MS);
    feed(b, s, 15, false, T0 + 820 * MS);
    if (!same(s.delivered, {10, 11, 12, 13, 14, 15}))
        return fail("timeout: keyframe was made stale by expired recovery deltas");
    return true;
}

bool test_abort_resolves_gap() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 10, true, T0);
    feed(b, s, 12, false, T0 + 10 * MS);
    b.on_frame_aborted(11, T0 + 20 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11}) || !same(s.delivered, {10}) || !b.needs_keyframe())
        return fail("abort: missing frame did not enter recovery");
    b.on_frame_aborted(11, T0 + 30 * MS, s.deliver_fn(), s.lost_fn());
    b.on_frame_aborted(5, T0 + 30 * MS, s.deliver_fn(), s.lost_fn());
    b.on_frame_aborted(20, T0 + 30 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11})) return fail("abort: repeated or unrelated abort reported loss");
    feed(b, s, 13, true, T0 + 40 * MS);
    b.on_frame_aborted(14, T0 + 50 * MS, s.deliver_fn(), s.lost_fn());
    if (!same(s.lost, {11, 14}) || b.last_delivered() != 13 || !b.needs_keyframe())
        return fail("abort: tail abort lost the last usable baseline");
    feed(b, s, 16, false, T0 + 60 * MS);
    feed(b, s, 17, false, T0 + 60 * MS);
    b.on_frame_aborted(17, T0 + 70 * MS, s.deliver_fn(), s.lost_fn());
    if (b.held_frames() != 1 || b.held_bytes() != VIDEO_FRAME_HEADER_SIZE + 100)
        return fail("abort: held frame not removed");
    feed(b, s, 15, true, T0 + 80 * MS);
    if (!same(s.delivered, {10, 13, 15, 16}) || b.needs_keyframe())
        return fail("abort: recovery keyframe did not resume delivery");
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
    if (!same(s.delivered, {10, 20, 21}) || !b.needs_keyframe())
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
    if (!same(s.delivered, {5000, 5001, 5002}) || !s.lost.empty())
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
    if (!same(s.delivered, {10, 11, 12}) || !s.lost.empty() || b.held_bytes() != 0)
        return fail("startup: delayed first keyframe and successors not delivered in order");

    // The first observed frame can be after the u32 wrap.
    b.reset();
    s = Sink{};
    feed(b, s, 0, false, T0);
    feed(b, s, 1, false, T0 + MS);
    feed(b, s, UINT32_MAX, true, T0 + 2 * MS);
    if (!same(s.delivered, {UINT32_MAX, 0, 1}) || !s.lost.empty())
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
    b.poll(T0 + 200 * MS, s.deliver_fn(), s.lost_fn());
    if (b.held_bytes() || b.held_frames() || !s.lost.empty())
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
    cfg.max_hold_ms = 10'000;
    VideoFrameReorderBuffer b(cfg);
    Sink s;
    feed(b, s, 10, true, T0);
    for (uint32_t seq = 12; seq <= 14; ++seq)
        feed(b, s, seq, false, T0 + seq * MS);
    if (b.held_frames() != 3 || !s.lost.empty()) return fail("overflow: below limit");
    feed(b, s, 15, false, T0 + 15 * MS);
    if (!same(s.lost, {11}) || !same(s.delivered, {10}) || !b.needs_keyframe())
        return fail("overflow: broken deltas advanced the baseline");
    if (b.held_frames() != 3 || b.held_bytes() != 3 * (VIDEO_FRAME_HEADER_SIZE + 100))
        return fail("overflow: bounded recovery accounting");
    feed(b, s, 12, true, T0 + 20 * MS);
    if (!same(s.delivered, {10, 12, 13, 14, 15}) || b.held_frames() || b.needs_keyframe())
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
    if (!same(s.lost, {11}) || !same(s.delivered, {10}) || b.held_bytes() != 828)
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
    feed(b, s, 3, true, T0 + 2 * MS);   // new share keyframe
    if (!same(s.delivered, {10, 3}) || !s.lost.empty()) return fail("reset: first frame after reset");
    // The old held frame is now a "newer with gap" frame; it is held, not delivered.
    feed(b, s, 12, false, T0 + 3 * MS);
    if (!same(s.delivered, {10, 3}) || b.held_frames() != 1) return fail("reset: stale frame handling");
    return true;
}

bool test_delayed_recovery_keyframes() {
    VideoFrameReorderBuffer b;
    Sink s;
    feed(b, s, 100, true, T0);
    // A large recovery keyframe completes after more than eight small deltas.
    // Repeating this pattern used to advance past every recovery point forever.
    for (uint32_t round = 0; round < 20; ++round) {
        const uint32_t key = 101 + round * 20;
        const int64_t now = T0 + round * 500 * MS;
        for (uint32_t delta = 1; delta <= 10; ++delta)
            feed(b, s, key + delta, false, now + delta * MS);
        feed(b, s, key, true, now + 200 * MS, 60'000);
        bool delivered_key = false;
        for (uint32_t seq : s.delivered) delivered_key |= seq == key;
        if (!delivered_key) return fail("recovery: delayed keyframe discarded after delta overflow");
        if (b.held_frames() > 8 || b.held_bytes() > 8 * 1024 * 1024)
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
    slow.poll(T0 + 160 * MS, stalled.deliver_fn(), stalled.lost_fn());
    if (!slow.needs_keyframe() || healthy.needs_keyframe())
        return fail("recovery: one viewer's loss affected another viewer");
    feed(slow, stalled, base - 100, true, T0 + 161 * MS);
    feed(slow, stalled, base, true, T0 + 162 * MS);
    if (!same(stalled.delivered, {base})) return fail("recovery: stale keyframe rolled back the stream");
    feed(slow, stalled, UINT32_MAX, true, T0 + 163 * MS);
    if (!same(stalled.delivered, {base, UINT32_MAX, 0}) || slow.needs_keyframe())
        return fail("recovery: delayed keyframe across wrap did not drain");
    feed(slow, stalled, 1, false, T0 + 164 * MS);
    if (!same(stalled.delivered, {base, UINT32_MAX, 0, 1}))
        return fail("recovery: playback did not continue after recovery");
    slow.reset();
    if (!slow.needs_keyframe() || slow.has_baseline()) return fail("recovery: reset state");
    return true;
}

} // namespace

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

    std::cout << "Video frame reorder buffer policy passed\n";
    return 0;
}
