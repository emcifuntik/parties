// Offline, deterministic tests for parties::client::DiagnosticsRecorder:
// freeze classification and the bucketed history behind the diagnostics
// overlay.

#include <client/diagnostics.h>

#include <cstdint>
#include <iostream>

using parties::VideoFrameHeader;
using parties::client::DiagConnectionSample;
using parties::client::DiagnosticsRecorder;
using parties::client::DiagServerReport;
using Cause = DiagnosticsRecorder::FreezeCause;
using Series = DiagnosticsRecorder::Series;

namespace {

constexpr int64_t T0 = 50'000'000;
constexpr int64_t MS = 1000;
constexpr uint32_t STREAM = 3;
constexpr int64_t FRAME_US = 16'667;

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

VideoFrameHeader frame(uint32_t seq, int64_t capture_us, bool keyframe = false) {
    VideoFrameHeader h;
    h.frame_seq = seq;
    h.timestamp = static_cast<uint32_t>(capture_us / 1000);
    h.flags = static_cast<uint8_t>((keyframe ? parties::VIDEO_FLAG_KEYFRAME : 0) |
                                   parties::VIDEO_FLAG_CAPTURE_TIME);
    return h;
}

// Plays `count` frames from `seq` captured and delivered every 16.7 ms,
// each presented 5 ms after delivery. Returns the next seq.
uint32_t play(DiagnosticsRecorder& rec, uint32_t seq, int count, int64_t& capture, int64_t& now) {
    for (int i = 0; i < count; ++i, ++seq) {
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        rec.on_frame_presented(STREAM, now + 5 * MS);
        capture += FRAME_US;
        now += FRAME_US;
    }
    return seq;
}

bool only_freeze(const DiagnosticsRecorder& rec, Cause cause, int64_t min_us, int64_t max_us, const char* what) {
    const auto f = rec.freezes();
    if (f.size() != 1) {
        std::cerr << what << ": " << f.size() << " freezes\n";
        return false;
    }
    if (f[0].cause != cause) {
        std::cerr << what << ": cause " << DiagnosticsRecorder::cause_name(f[0].cause) << "\n";
        return false;
    }
    if (f[0].duration_us < min_us || f[0].duration_us > max_us) {
        std::cerr << what << ": duration " << f[0].duration_us << "\n";
        return false;
    }
    return true;
}

// Frames captured on time but held 600 ms in the network.
bool test_network_freeze() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    if (!rec.freezes().empty()) return fail("network: freeze in smooth playback");
    // 36 frames captured during the stall arrive together after 600 ms.
    now += 600 * MS;
    for (int i = 0; i < 36; ++i, ++seq) {
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        rec.on_frame_presented(STREAM, now + 5 * MS);
        capture += FRAME_US;
        now += 1 * MS;
    }
    return only_freeze(rec, Cause::Network, 600 * MS, 640 * MS, "network");
}

// The sharer produced nothing for 800 ms (static screen).
bool test_sender_freeze() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    capture += 800 * MS;
    now += 800 * MS;
    play(rec, seq, 10, capture, now);
    return only_freeze(rec, Cause::Sender, 800 * MS, 840 * MS, "sender");
}

// Frames dropped by the server; playback resumes at a keyframe 700 ms later.
bool test_lost_freeze() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    seq += 42;
    capture += 42 * FRAME_US;
    now += 700 * MS;
    rec.on_frame_delivered(STREAM, frame(seq, capture, true), 60'000, now);
    rec.on_frame_presented(STREAM, now + 5 * MS);
    if (!only_freeze(rec, Cause::Lost, 700 * MS, 740 * MS, "lost")) return false;
    if (rec.freezes()[0].frames_lost != 42) return fail("lost: frame count");
    const auto s = rec.streams(now + 10 * MS);
    if (s.size() != 1 || s[0].frames_lost != 42 || s[0].keyframes != 1) return fail("lost: summary");
    return true;
}

// Frames reached the decoder on time; none reached the screen for 400 ms.
bool test_decode_freeze() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    const int64_t stall_start = now;
    for (int i = 0; i < 24; ++i, ++seq) {
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        capture += FRAME_US;
        now += FRAME_US;
    }
    rec.on_frame_presented(STREAM, now);
    (void)stall_start;
    return only_freeze(rec, Cause::Decode, 390 * MS, 430 * MS, "decode");
}

// A 130 ms network hitch is short of the old 200 ms threshold but visible.
bool test_short_network_hitch() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    now += 130 * MS;
    for (int i = 0; i < 8; ++i, ++seq) {
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        rec.on_frame_presented(STREAM, now + 5 * MS);
        capture += FRAME_US;
        now += 1 * MS;
    }
    return only_freeze(rec, Cause::Network, 130 * MS, 160 * MS, "short network hitch");
}

// A 150 ms pause in what the sharer captured (a static screen) is not shown.
bool test_short_sender_pause_ignored() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    capture += 150 * MS;
    now += 150 * MS;
    play(rec, seq, 10, capture, now);
    if (!rec.freezes().empty()) return fail("short sender pause: recorded");
    return true;
}

// One frame lost, the rest keep arriving on time, but the decoder waits for
// the keyframe that comes 250 ms later: a lost-frames freeze, not decode.
bool test_lost_without_delivery_gap() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = play(rec, 0, 60, capture, now);
    ++seq;
    capture += FRAME_US;
    now += FRAME_US;
    for (int i = 0; i < 15; ++i, ++seq) {   // delivered, not decodable
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        capture += FRAME_US;
        now += FRAME_US;
    }
    rec.on_frame_delivered(STREAM, frame(seq, capture, true), 60'000, now);
    rec.on_frame_presented(STREAM, now + 5 * MS);
    if (!only_freeze(rec, Cause::Lost, 250 * MS, 300 * MS, "lost without delivery gap")) return false;
    if (rec.freezes()[0].frames_lost != 1) return fail("lost without delivery gap: frame count");
    return true;
}

// The render loop stalls 150 ms: one app-wide render freeze, and the video's
// presentation gap it caused is not counted again as decode.
bool test_render_stall() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    for (int i = 0; i < 60; ++i) {
        rec.on_render_tick(now, true);
        rec.on_frame_delivered(STREAM, frame(static_cast<uint32_t>(i), capture), 20'000, now);
        rec.on_frame_presented(STREAM, now + 1 * MS);
        capture += FRAME_US;
        now += FRAME_US;
    }
    uint32_t seq = 60;
    // Frames keep arriving; nothing renders for 150 ms.
    for (int i = 0; i < 9; ++i, ++seq) {
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        capture += FRAME_US;
        now += FRAME_US;
    }
    rec.on_render_tick(now, true);
    rec.on_frame_presented(STREAM, now + 1 * MS);
    const auto f = rec.freezes();
    if (f.size() != 1 || f[0].cause != Cause::Render || f[0].stream != DiagnosticsRecorder::APP_STREAM ||
        f[0].duration_us < 150 * MS || f[0].duration_us > 180 * MS)
        return fail("render stall: not recorded once for the app");
    return true;
}

// A minimized window draws nothing for 5 s: neither a render stall nor a
// video freeze when it comes back.
bool test_minimized_not_frozen() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    uint32_t seq = 0;
    for (int i = 0; i < 60; ++i, ++seq) {
        rec.on_render_tick(now, true);
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        rec.on_frame_presented(STREAM, now + 1 * MS);
        capture += FRAME_US;
        now += FRAME_US;
    }
    for (int i = 0; i < 300; ++i, ++seq) {
        rec.on_render_tick(now, false);
        rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
        capture += FRAME_US;
        now += FRAME_US;
    }
    rec.on_render_tick(now, true);
    rec.on_frame_delivered(STREAM, frame(seq, capture), 20'000, now);
    rec.on_frame_presented(STREAM, now + 1 * MS);
    if (!rec.freezes().empty()) return fail("minimized: recorded as a freeze");
    return true;
}

// A share restart (sequence back at 0) is not counted as lost frames.
bool test_restart_not_lost() {
    DiagnosticsRecorder rec;
    int64_t capture = T0, now = T0 + 30 * MS;
    play(rec, 5000, 30, capture, now);
    play(rec, 0, 30, capture, now);
    const auto s = rec.streams(now);
    if (s.size() != 1 || s[0].frames_lost != 0) return fail("restart: counted as loss");
    return true;
}

// Bucketed connection history: rates from the cumulative counters, loss
// from the packet deltas, the server's view next to ours.
bool test_history() {
    DiagnosticsRecorder rec;
    DiagConnectionSample c;
    c.valid = true;
    c.rtt_us = 40'000;
    DiagServerReport srv;
    rec.sample(T0, c, srv);
    // 250 ms: 125 KB down (4 Mbps), 12.5 KB up (400 kbps), 2 of 100 packets lost.
    c.recv_bytes += 125'000;
    c.sent_bytes += 12'500;
    c.sent_packets += 100;
    c.lost_packets += 2;
    srv.valid = true;
    srv.loss_permille = 35;
    srv.srtt_us = 42'000;
    rec.sample(T0 + 100 * MS, c, srv);   // inside the bucket: no-op
    if (rec.revision() != 0) return fail("history: bucket closed early");
    rec.sample(T0 + 250 * MS, c, srv);
    if (rec.revision() != 1) return fail("history: bucket not closed");
    auto near = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
    if (!near(rec.history(Series::DownKbps).back(), 4000.0f)) return fail("history: down rate");
    if (!near(rec.history(Series::UpKbps).back(), 400.0f)) return fail("history: up rate");
    if (!near(rec.history(Series::UpLossPct).back(), 2.0f)) return fail("history: up loss");
    if (!near(rec.history(Series::DownLossPct).back(), 3.5f)) return fail("history: down loss");
    if (!near(rec.history(Series::RttMs).back(), 40.0f)) return fail("history: rtt");
    if (!near(rec.history(Series::ServerRttMs).back(), 42.0f)) return fail("history: server rtt");
    if (rec.history(Series::DownKbps)[0] != 0.0f) return fail("history: oldest bucket not empty");
    return true;
}

// Presented fps and freeze marks per bucket.
bool test_stream_buckets() {
    DiagnosticsRecorder rec;
    DiagConnectionSample c;
    rec.sample(T0, c, {});
    int64_t capture = T0, now = T0;
    play(rec, 0, 15, capture, now);   // 250 ms at 60 fps
    rec.sample(T0 + 250 * MS, c, {});
    const float fps = rec.stream_fps(STREAM).back();
    if (fps < 56.0f || fps > 64.0f) return fail("buckets: fps");
    // Nothing presented for the next 500 ms: both buckets marked frozen.
    rec.sample(T0 + 500 * MS, c, {});
    rec.sample(T0 + 750 * MS, c, {});
    const auto fr = rec.stream_freezes(STREAM);
    if (fr[DiagnosticsRecorder::BUCKETS - 1] != 1.0f) return fail("buckets: ongoing stall not marked");
    if (fr[DiagnosticsRecorder::BUCKETS - 3] != 0.0f) return fail("buckets: smooth bucket marked");
    const auto s = rec.streams(T0 + 750 * MS);
    if (s.empty() || s[0].stalled_us < 400 * MS) return fail("buckets: stall not reported");
    return true;
}

} // namespace

int main() {
    if (!test_network_freeze()) return 1;
    if (!test_sender_freeze()) return 1;
    if (!test_lost_freeze()) return 1;
    if (!test_decode_freeze()) return 1;
    if (!test_short_network_hitch()) return 1;
    if (!test_short_sender_pause_ignored()) return 1;
    if (!test_lost_without_delivery_gap()) return 1;
    if (!test_render_stall()) return 1;
    if (!test_minimized_not_frozen()) return 1;
    if (!test_restart_not_lost()) return 1;
    if (!test_history()) return 1;
    if (!test_stream_buckets()) return 1;
    std::cout << "Diagnostics recorder passed\n";
    return 0;
}
