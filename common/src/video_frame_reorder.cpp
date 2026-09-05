#include <parties/video_frame_reorder.h>

#include <algorithm>
#include <utility>

// Video ingress and viewer reorder buffer. See the header for the behaviour summary.
//
// Keying scheme
// -------------
// After the first keyframe, held frames are strictly newer than last_delivered_, so
// they all lie inside the half-range (last_delivered_, last_delivered_ + 2^31)
// and their wrap-safe order equals the order of
//     key = video_seq_distance(frame_seq, last_delivered_)      (1 .. 2^31-1)
// std::map<int64_t, Held> keyed that way therefore iterates from the oldest
// missing-successor to the newest frame. The base moves whenever
// last_delivered_ advances, so every operation that advances it ends in
// drain(), which re-keys the surviving entries against the new base (at most
// max_held_frames entries, i.e. trivially cheap). Inside drain() the entries
// are matched by their actual frame_seq, never by the (possibly stale) key.
//
// Invariants kept between calls:
//   * before the first keyframe, held_ contains bounded startup delta frames;
//   * once have_last_, every held frame is newer than last_delivered_;
//   * held_bytes_ == sum of held frame sizes;
//   * gap_since_us_ == earliest arrival time among held frames, 0 when none
//     (a gap is open exactly when held_ is non-empty).
//
// This translation unit is also compiled stand-alone by
// tests/parties_video_frame_reorder_test, so it must not depend on the
// logging or tracing facilities of parties_common.

namespace parties {

namespace {

// Re-key a held map against a new base. Templated so the private Held type
// never has to be named outside the class.
template <class Map>
void rekey_held(Map& held, uint32_t base) {
    if (held.empty()) return;
    Map rebased;
    for (auto& [key, h] : held) {
        (void)key;
        const int64_t k = video_seq_distance(h.hdr.frame_seq, base);
        rebased.emplace(k, std::move(h));
    }
    held.swap(rebased);
}

template <class Map>
int64_t earliest_arrival(const Map& held) {
    int64_t earliest = 0;
    for (const auto& [key, h] : held) {
        (void)key;
        if (earliest == 0 || h.arrived_us < earliest) earliest = h.arrived_us;
    }
    return earliest;
}

} // namespace

VideoFrameReorderBuffer::VideoFrameReorderBuffer()
    : VideoFrameReorderBuffer(VideoFrameReorderConfig{}) {}

VideoFrameReorderBuffer::VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg)
    : cfg_(cfg) {
    if (cfg_.max_held_frames == 0) cfg_.max_held_frames = 1;
}

void VideoFrameReorderBuffer::reset() {
    have_last_ = false;
    last_delivered_ = 0;
    held_.clear();
    held_bytes_ = 0;
    gap_since_us_ = 0;
}

int64_t VideoFrameReorderBuffer::key_for(uint32_t seq) const {
    return static_cast<int64_t>(video_seq_distance(seq, last_delivered_));
}

void VideoFrameReorderBuffer::on_frame(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame,
                                       int64_t now_us, const DeliverFn& deliver, const LostFn& lost) {
    const uint32_t seq = hdr.frame_seq;

    // A small delta can finish before its much larger initial keyframe.
    // Keep those successors without committing a decode baseline: otherwise
    // the delayed keyframe would be discarded as stale.
    if (!have_last_) {
        if (!hdr.keyframe()) {
            if (held_.empty()) last_delivered_ = seq;
            const int64_t key = key_for(seq);
            if (held_.contains(key)) return;
            held_bytes_ += frame.size();
            held_.emplace(key, Held{hdr, std::move(frame), now_us});
            while (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
                held_bytes_ -= held_.begin()->second.frame.size();
                held_.erase(held_.begin());
            }
            gap_since_us_ = earliest_arrival(held_);
            return;
        }
        for (auto it = held_.begin(); it != held_.end();) {
            if (!video_seq_newer(it->second.hdr.frame_seq, seq)) {
                held_bytes_ -= it->second.frame.size();
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        have_last_ = true;
        last_delivered_ = seq;
        rekey_held(held_, seq);
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(deliver);
        return;
    }

    // The expected next frame: deliver and release anything queued behind it.
    if (seq == last_delivered_ + 1) {
        last_delivered_ = seq;
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(deliver);
        return;
    }

    if (!video_seq_newer(seq, last_delivered_)) {
        // Independent reliable streams may arrive arbitrarily late. A large
        // backward jump is not evidence of a restart; only the owner's share
        // lifecycle may reset the baseline.
        return;
    }

    // Newer than last_delivered_ + 1: there is a gap in front of it.
    if (hdr.keyframe()) {
        // Nothing before a keyframe is needed: throw away every held frame
        // that is not newer than it and resume from it. The gap is NOT
        // reported through `lost`: the keyframe re-anchors the decoder by
        // itself, and this is exactly how the server resumes a throttled
        // viewer — a loss report here would only trigger a pointless PLI.
        const int64_t key = key_for(seq);
        auto end = held_.upper_bound(key);
        for (auto it = held_.begin(); it != end; ++it)
            held_bytes_ -= it->second.frame.size();
        held_.erase(held_.begin(), end);
        last_delivered_ = seq;
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(deliver);
        return;
    }

    // Delta frame behind a gap: hold it until the gap closes or expires.
    const int64_t key = key_for(seq);
    if (held_.find(key) != held_.end()) return;   // duplicate of a held frame
    if (held_.empty()) gap_since_us_ = now_us;      // the wait for last+1 starts now
    held_bytes_ += frame.size();
    held_.emplace(key, Held{hdr, std::move(frame), now_us});

    // Bound the buffer: each skip_gap() delivers at least the oldest held
    // frame, so the loop always terminates.
    while (!held_.empty() &&
           (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes))
        skip_gap(now_us, deliver, lost);
}

void VideoFrameReorderBuffer::on_frame_aborted(uint32_t frame_seq, int64_t /*now_us*/,
                                               const DeliverFn& deliver, const LostFn& lost) {
    if (!have_last_) return;   // nothing waits on it

    if (frame_seq == last_delivered_ + 1) {
        // The frame we were waiting for will never come: report it and move on.
        lost(frame_seq);
        last_delivered_ = frame_seq;
        drain(deliver);
        return;
    }

    if (!video_seq_newer(frame_seq, last_delivered_)) return;   // already behind us

    // A held frame cannot be aborted (it completed), but be tolerant.
    auto it = held_.find(key_for(frame_seq));
    if (it == held_.end()) return;
    held_bytes_ -= it->second.frame.size();
    held_.erase(it);
    gap_since_us_ = earliest_arrival(held_);
}

void VideoFrameReorderBuffer::poll(int64_t now_us, const DeliverFn& deliver, const LostFn& lost) {
    const int64_t max_hold_us = static_cast<int64_t>(cfg_.max_hold_ms) * 1000;
    if (!have_last_) {
        for (auto it = held_.begin(); it != held_.end();) {
            if (now_us - it->second.arrived_us >= max_hold_us) {
                held_bytes_ -= it->second.frame.size();
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        gap_since_us_ = earliest_arrival(held_);
        return;
    }
    // A second gap left behind by skip_gap() may already have expired too.
    while (!held_.empty() && now_us - gap_since_us_ >= max_hold_us)
        skip_gap(now_us, deliver, lost);
}

void VideoFrameReorderBuffer::deliver_one(Held&& h, const DeliverFn& deliver) {
    if (deliver) deliver(h.hdr, std::move(h.frame));
}

void VideoFrameReorderBuffer::drain(const DeliverFn& deliver) {
    // Deliver consecutive held frames. Entries are matched by frame_seq: the
    // keys are stale while last_delivered_ moves, but their order is not.
    while (!held_.empty()) {
        auto it = held_.begin();
        if (it->second.hdr.frame_seq != last_delivered_ + 1) break;
        Held h = std::move(it->second);
        held_.erase(it);
        held_bytes_ -= h.frame.size();
        last_delivered_ = h.hdr.frame_seq;
        deliver_one(std::move(h), deliver);
    }
    rekey_held(held_, last_delivered_);
    gap_since_us_ = earliest_arrival(held_);
}

void VideoFrameReorderBuffer::skip_gap(int64_t /*now_us*/, const DeliverFn& deliver, const LostFn& lost) {
    if (held_.empty()) return;
    // Give up on the missing frame(s): report the first one once and resume
    // right before the oldest held frame so drain() releases it. The decode
    // gate sees the discontinuity and the caller requests a keyframe.
    lost(last_delivered_ + 1);
    last_delivered_ = held_.begin()->second.hdr.frame_seq - 1;
    drain(deliver);
}

} // namespace parties
