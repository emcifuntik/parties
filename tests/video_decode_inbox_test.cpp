#include <client/video_decode_inbox.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

using parties::ParsedVideoFrame;
using parties::VideoFrameHeader;
using parties::client::DecodeWork;
using parties::client::KeyframeRecovery;
using parties::client::kVideoDecodeStaleFlushPliLimit;
using parties::client::StreamOrigin;
using parties::client::VideoDecodeInbox;

namespace {

int fail(const char* what) {
    std::cerr << "FAIL: " << what << "\n";
    return 1;
}

ParsedVideoFrame frame(uint32_t seq, bool keyframe) {
    const std::vector<uint8_t> encoded{0x12, 0x34};
    return ParsedVideoFrame::assemble(VideoFrameHeader{.frame_seq = seq, .keyframe = keyframe,
                                                       .width = 64, .height = 64},
                                      encoded);
}

std::deque<DecodeWork> take(VideoDecodeInbox& inbox) {
    std::stop_source live;
    return inbox.wait_batch(live.get_token()).value();
}

std::vector<uint32_t> seqs(const std::deque<DecodeWork>& batch) {
    std::vector<uint32_t> out;
    for (const auto& work : batch) out.push_back(work.frame.header().frame_seq);
    return out;
}

int discontinuity_verdict() {
    VideoDecodeInbox remote(StreamOrigin::RemoteSharer);
    if (remote.push(frame(9, false), 0) != KeyframeRecovery::None)
        return fail("delta before the first keyframe asked for recovery");
    if (remote.push(frame(10, true), 1) != KeyframeRecovery::None ||
        remote.push(frame(11, false), 2) != KeyframeRecovery::None)
        return fail("contiguous frames asked for recovery");
    if (remote.push(frame(13, false), 3) != KeyframeRecovery::RequestFromSharer)
        return fail("remote gap did not request a keyframe from the sharer");
    if (remote.push(frame(14, false), 4) != KeyframeRecovery::None)
        return fail("delta while awaiting the keyframe asked again");
    if (remote.push(frame(20, true), 5) != KeyframeRecovery::None)
        return fail("recovery keyframe asked for recovery");
    const auto batch = take(remote);
    if (seqs(batch) != std::vector<uint32_t>{20})
        return fail("discontinuity did not discard the queued frames");
    if (!batch.front().recovery_keyframe || batch.front().arrival_us != 5)
        return fail("keyframe ending the wait was not marked as recovery");

    VideoDecodeInbox self(StreamOrigin::SelfPreview);
    (void)self.push(frame(1, true), 0);
    if (self.push(frame(3, false), 1) != KeyframeRecovery::AwaitLocalEncoder)
        return fail("self-preview gap did not wait for the local encoder");
    return 0;
}

int stale_flush_limit_one_shot() {
    for (const auto origin : {StreamOrigin::RemoteSharer, StreamOrigin::SelfPreview}) {
        const auto ask = origin == StreamOrigin::RemoteSharer ? KeyframeRecovery::RequestFromSharer
                                                              : KeyframeRecovery::AwaitLocalEncoder;
        VideoDecodeInbox inbox(origin);
        for (uint32_t i = 0; i < kVideoDecodeStaleFlushPliLimit; ++i) {
            if (inbox.resync_after_stale_backlog() != ask)
                return fail("stale flush within the limit did not ask for a keyframe");
        }
        if (inbox.stale_flushes_suppressed())
            return fail("suppressed before the limit was crossed");
        if (inbox.resync_after_stale_backlog() != KeyframeRecovery::AwaitPeriodicKeyframe)
            return fail("crossing the limit did not announce the periodic keyframe wait");
        if (!inbox.stale_flushes_suppressed())
            return fail("not suppressed after crossing the limit");
        for (int i = 0; i < 3; ++i) {
            if (inbox.resync_after_stale_backlog() != KeyframeRecovery::Suppressed)
                return fail("limit+1 verdict was not one-shot");
        }
    }

    VideoDecodeInbox inbox(StreamOrigin::RemoteSharer);
    (void)inbox.push(frame(1, true), 0);
    (void)inbox.push(frame(2, false), 0);
    if (inbox.resync_after_stale_backlog() != KeyframeRecovery::RequestFromSharer)
        return fail("first stale flush did not request a keyframe");
    if (inbox.push(frame(3, false), 0) != KeyframeRecovery::None)
        return fail("delta after a stale flush was not silently dropped");
    (void)inbox.push(frame(30, true), 10);
    if (seqs(take(inbox)) != std::vector<uint32_t>{30})
        return fail("stale flush did not discard the queue until the next keyframe");
    return 0;
}

int self_preview_never_requests_from_sharer() {
    VideoDecodeInbox self(StreamOrigin::SelfPreview);
    if (self.reset_after_decoder_failure() != KeyframeRecovery::AwaitLocalEncoder)
        return fail("self-preview decoder failure did not wait for the local encoder");
    if (self.wants_pli_retry())
        return fail("self-preview wants a PLI retry");

    VideoDecodeInbox remote(StreamOrigin::RemoteSharer);
    for (int i = 0; i < 5; ++i) {
        if (remote.reset_after_decoder_failure() != KeyframeRecovery::RequestFromSharer)
            return fail("remote decoder failure did not request a keyframe");
    }
    if (remote.stale_flushes_suppressed())
        return fail("decoder failures counted toward the stale-flush limit");
    return 0;
}

int stop_wakes_wait() {
    VideoDecodeInbox inbox(StreamOrigin::RemoteSharer);
    std::promise<bool> woke;
    auto result = woke.get_future();
    std::jthread consumer([&](std::stop_token stop) {
        woke.set_value(!inbox.wait_batch(stop).has_value());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    consumer.request_stop();
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::cerr << "FAIL: stop did not wake a blocked wait\n";
        std::_Exit(1);
    }
    if (!result.get()) return fail("stopped wait returned a batch");
    consumer.join();

    (void)inbox.push(frame(1, true), 0);
    std::stop_source stopped;
    stopped.request_stop();
    if (inbox.wait_batch(stopped.get_token()).has_value())
        return fail("stopped wait handed out queued work");
    return 0;
}

int pli_retry() {
    VideoDecodeInbox inbox(StreamOrigin::RemoteSharer);
    if (!inbox.wants_pli_retry()) return fail("fresh remote stream does not want a keyframe");
    (void)inbox.push(frame(1, true), 0);
    if (inbox.wants_pli_retry()) return fail("still wants a keyframe after one arrived");
    (void)inbox.push(frame(3, false), 0);
    if (!inbox.wants_pli_retry()) return fail("gap did not re-arm the retry");
    (void)inbox.push(frame(4, true), 0);
    if (inbox.wants_pli_retry()) return fail("keyframe did not clear the retry");

    for (uint32_t i = 0; i <= kVideoDecodeStaleFlushPliLimit; ++i)
        (void)inbox.resync_after_stale_backlog();
    if (inbox.wants_pli_retry()) return fail("suppressed stream still retries PLIs");
    return 0;
}

}

int main() {
    if (discontinuity_verdict()) return 1;
    if (stale_flush_limit_one_shot()) return 1;
    if (self_preview_never_requests_from_sharer()) return 1;
    if (stop_wakes_wait()) return 1;
    if (pli_retry()) return 1;
    std::cout << "Video decode inbox passed\n";
    return 0;
}
