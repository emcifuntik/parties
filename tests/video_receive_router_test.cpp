// Offline, deterministic tests for parties::VideoReceiveRouter: watch
// lifecycle and keyframe-request pacing on the viewer side.

#include <parties/video_receive_router.h>

#include <cstdint>
#include <iostream>
#include <vector>

using parties::VideoFrameHeader;
using parties::VideoReceiveRouter;
using parties::VIDEO_FLAG_KEYFRAME;
using parties::VIDEO_FRAME_HEADER_SIZE;

namespace {

constexpr int64_t T0 = 10'000'000;
constexpr int64_t MS = 1000;
constexpr uint32_t STREAM = 7;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

std::vector<uint8_t> frame(uint32_t seq, bool keyframe) {
    VideoFrameHeader hdr;
    hdr.frame_seq = seq;
    hdr.flags = keyframe ? VIDEO_FLAG_KEYFRAME : 0;
    std::vector<uint8_t> buf(VIDEO_FRAME_HEADER_SIZE + 32, 0x5A);
    hdr.write(buf.data());
    return buf;
}

VideoFrameHeader header(uint32_t seq, bool keyframe) {
    VideoFrameHeader hdr;
    hdr.frame_seq = seq;
    hdr.flags = keyframe ? VIDEO_FLAG_KEYFRAME : 0;
    return hdr;
}

struct Harness {
    std::vector<uint32_t> delivered;
    std::vector<int64_t> requests;   // request times
    int64_t now = T0;
    static parties::VideoReceiveRouterConfig inline_cfg() {
        parties::VideoReceiveRouterConfig cfg;
        cfg.playout = false;   // deliver synchronously: these tests are about routing and pacing
        return cfg;
    }
    VideoReceiveRouter router{
        [this](uint32_t, const VideoFrameHeader& hdr, std::vector<uint8_t>&&) {
            delivered.push_back(hdr.frame_seq);
        },
        [this](uint32_t) { requests.push_back(now); }, inline_cfg()};
};

// Frames for a stream nobody watches are dropped, and a reopened stream starts
// from scratch: a restarted share (counter back at 0) plays at once.
bool test_lifecycle() {
    Harness h;
    h.router.on_frame(STREAM, frame(1, true), h.now);
    if (!h.delivered.empty()) return fail("lifecycle: delivered without a watch");
    h.router.open(STREAM);
    h.router.on_frame(STREAM, frame(5000, true), h.now);
    h.router.on_frame(STREAM, frame(5001, false), h.now);
    h.router.close(STREAM);
    h.router.on_frame(STREAM, frame(5002, false), h.now);
    h.router.open(STREAM);
    h.router.on_frame(STREAM, frame(0, true), h.now);
    h.router.on_frame(STREAM, frame(1, false), h.now);
    if (h.delivered != std::vector<uint32_t>{5000, 5001, 0, 1})
        return fail("lifecycle: reopen did not start from the new share");
    return true;
}

// While a stream stays broken, requests back off 500 ms, 1 s, 2 s, 4 s, 4 s;
// a delivered keyframe resets the pacing.
bool test_backoff() {
    Harness h;
    h.router.open(STREAM);
    for (int64_t t = 0; t <= 12'000; t += 50) {
        h.now = T0 + t * MS;
        h.router.tick(h.now);
    }
    const std::vector<int64_t> expected = {
        T0, T0 + 500 * MS, T0 + 1'500 * MS, T0 + 3'500 * MS, T0 + 7'500 * MS, T0 + 11'500 * MS};
    if (h.requests != expected) return fail("backoff: unexpected request schedule");
    h.router.on_frame(STREAM, frame(1, true), h.now);
    // A later gap is requested immediately, then paced from the base again.
    h.router.on_frame_aborted(STREAM, 2, true, h.now);
    h.now += 100 * MS;
    h.router.tick(h.now);
    h.now += 450 * MS;
    h.router.tick(h.now);
    if (h.requests.size() != expected.size() + 2 ||
        h.requests[expected.size()] != T0 + 12'000 * MS ||
        h.requests[expected.size() + 1] != T0 + 12'550 * MS)
        return fail("backoff: a delivered keyframe did not reset the pacing");
    const auto stats = h.router.stats(STREAM);
    if (stats.keyframes_delivered != 1 || stats.requests_sent != h.requests.size())
        return fail("backoff: stats");
    return true;
}

// No request while a keyframe is already arriving, not even when the decoder
// asks or a frame is aborted before its header.
bool test_keyframe_arriving() {
    Harness h;
    h.router.open(STREAM);
    h.router.on_frame_started(STREAM, header(1, true), h.now);
    h.router.tick(h.now, [](uint32_t) { return true; });
    h.router.on_frame_aborted(STREAM, 0, false, h.now);
    if (!h.requests.empty()) return fail("arriving: requested while a keyframe is arriving");
    h.router.on_frame(STREAM, frame(1, true), h.now);
    h.router.on_frame_aborted(STREAM, 0, false, h.now + 10 * MS);
    if (h.requests.size() != 1) return fail("arriving: a headerless abort must request a keyframe");
    return true;
}

// A decoder that lost its reference is served through the same pacing.
bool test_decoder_requests_paced() {
    Harness h;
    h.router.open(STREAM);
    h.router.on_frame(STREAM, frame(1, true), h.now);
    for (int i = 0; i < 10; ++i) h.router.request_keyframe(STREAM, h.now + i * MS);
    if (h.requests.size() != 1) return fail("decoder: requests were not paced");
    return true;
}

// Frames captured every 16 ms; a retransmission holds frame 10 back by
// 150 ms and frames 11..19 arrive in the same instant behind it. The clock
// must spread them out again at their capture spacing behind a cushion.
bool test_playout_clock_spreads_clumps() {
    parties::VideoPlayoutClock clock;
    auto hdr = [](uint32_t seq) {
        VideoFrameHeader h;
        h.frame_seq = seq;
        h.timestamp = 1000 + seq * 16;
        h.flags = parties::VIDEO_FLAG_CAPTURE_TIME;
        return h;
    };
    const int64_t transit = 40 * MS;
    int64_t last_at = 0;
    for (uint32_t seq = 0; seq < 10; ++seq) {
        const int64_t arrival = (1000 + seq * 16) * MS + transit;
        const int64_t at = clock.playout_time(hdr(seq), arrival);
        if (at != arrival) return fail("playout: an even stream must play on arrival");
        last_at = at;
    }
    const int64_t clump = (1000 + 10 * 16) * MS + transit + 150 * MS;
    std::vector<int64_t> plays;
    for (uint32_t seq = 10; seq < 20; ++seq) plays.push_back(clock.playout_time(hdr(seq), clump));
    if (plays.front() != clump) return fail("playout: the late frame must play on arrival");
    // The frames that are late even with the cushion play at once; the rest
    // are spread out again at their capture spacing.
    for (size_t i = 1; i < plays.size(); ++i)
        if (plays[i] < plays[i - 1]) return fail("playout: out of order");
    for (size_t i = 3; i < plays.size(); ++i)
        if (plays[i] - plays[i - 1] != 16 * MS) return fail("playout: clumped frames not spread at capture spacing");
    if (plays.back() - plays.front() < 7 * 16 * MS) return fail("playout: clump still delivered at once");
    // The cushion now covers that jitter: the next ordinary frame waits for it.
    const int64_t arrival = (1000 + 20 * 16) * MS + transit;
    const int64_t at = clock.playout_time(hdr(20), arrival);
    if (at <= arrival || at - arrival > 300 * MS) return fail("playout: cushion not applied or not capped");
    if (clock.delay_us() < 100 * MS) return fail("playout: delay did not follow the jitter");
    // Frames without a capture time play on arrival.
    VideoFrameHeader plain;
    plain.frame_seq = 21;
    if (clock.playout_time(plain, arrival + 5 * MS) != arrival + 5 * MS)
        return fail("playout: legacy frame delayed");
    (void)last_at;
    return true;
}

} // namespace

int main() {
    if (!test_playout_clock_spreads_clumps()) return 1;
    if (!test_lifecycle()) return 1;
    if (!test_backoff()) return 1;
    if (!test_keyframe_arriving()) return 1;
    if (!test_decoder_requests_paced()) return 1;
    std::cout << "Video receive router passed\n";
    return 0;
}
