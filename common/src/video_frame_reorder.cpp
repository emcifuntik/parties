#include <parties/video_frame_reorder.h>

#include <algorithm>
#include <iterator>
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
//   * while waiting for a keyframe, held_ contains bounded delta frames;
//   * once have_last_, every held frame is newer than last_delivered_;
//   * held_bytes_ == sum of held frame sizes;
//   * gap_since_us_ == earliest arrival time among held frames, 0 when none
//     (recovery deltas never advance the last usable decode baseline);
//   * loss_reported_ implies awaiting_keyframe_ (cleared whenever delivery
//     resumes);
//   * every aborted_ entry is newer than last_delivered_ once have_last_.
//
// This translation unit is also compiled stand-alone by
// tests/parties_video_frame_reorder_test, so it must not depend on the
// logging or tracing facilities of parties_common.

namespace parties {

namespace {

constexpr size_t MAX_ABORTED_TRACKED = 64;

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
    if (cfg_.max_hold_ms < cfg_.min_hold_ms) cfg_.max_hold_ms = cfg_.min_hold_ms;
}

void VideoFrameReorderBuffer::reset() {
    have_last_ = false;
    awaiting_keyframe_ = true;
    loss_reported_ = false;
    last_delivered_ = 0;
    held_.clear();
    held_bytes_ = 0;
    gap_since_us_ = 0;
    inflight_key_ = false;
    inflight_key_seq_ = 0;
    inflight_key_since_us_ = 0;
    aborted_.clear();
}

int64_t VideoFrameReorderBuffer::hold_us() const {
    int64_t hold = static_cast<int64_t>(cfg_.min_hold_ms) * 1000;
    if (rtt_us_ > 0) {
        hold = (std::max)(hold, static_cast<int64_t>(rtt_us_) * cfg_.rtt_hold_factor +
                                    static_cast<int64_t>(cfg_.hold_extra_ms) * 1000);
    }
    return (std::min)(hold, static_cast<int64_t>(cfg_.max_hold_ms) * 1000);
}

bool VideoFrameReorderBuffer::keyframe_in_flight(int64_t now_us) const {
    if (!inflight_key_) return false;
    // A keyframe stream that has been arriving for this long is presumed
    // stuck: stop trusting it so the owner asks for another one.
    const int64_t trust_us = (std::max)(4 * hold_us(), int64_t{2'000'000});
    return now_us - inflight_key_since_us_ < trust_us;
}

int64_t VideoFrameReorderBuffer::key_for(uint32_t seq) const {
    return static_cast<int64_t>(video_seq_distance(seq, last_delivered_));
}

void VideoFrameReorderBuffer::on_frame_started(const VideoFrameHeader& hdr, int64_t now_us) {
    if (!hdr.keyframe()) return;
    const uint32_t seq = hdr.frame_seq;
    if (have_last_ && !video_seq_newer(seq, last_delivered_)) return;
    if (!inflight_key_ || video_seq_newer(seq, inflight_key_seq_)) {
        inflight_key_ = true;
        inflight_key_seq_ = seq;
        inflight_key_since_us_ = now_us;
    }
}

void VideoFrameReorderBuffer::forget_inflight_through(uint32_t seq) {
    if (inflight_key_ && !video_seq_newer(inflight_key_seq_, seq)) inflight_key_ = false;
}

bool VideoFrameReorderBuffer::next_is_aborted() const {
    return std::find(aborted_.begin(), aborted_.end(), last_delivered_ + 1) != aborted_.end();
}

void VideoFrameReorderBuffer::on_frame(const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame,
                                       int64_t now_us, const DeliverFn& deliver, const LostFn& lost) {
    const uint32_t seq = hdr.frame_seq;

    // Old frames never move the baseline back, keyframes included: a restarted
    // share is announced explicitly and its owner calls reset().
    if (have_last_ && !video_seq_newer(seq, last_delivered_)) return;

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
            bound_held();
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
        awaiting_keyframe_ = false;
        loss_reported_ = false;
        last_delivered_ = seq;
        aborted_.clear();
        rekey_held(held_, seq);
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(now_us, deliver, lost);
        return;
    }

    // The expected next frame: deliver and release anything queued behind it.
    // It also closes a gap that already requested a keyframe: the chain from
    // last_delivered_ is intact, so a late retransmission resumes playback.
    if (seq == last_delivered_ + 1) {
        awaiting_keyframe_ = false;
        loss_reported_ = false;
        last_delivered_ = seq;
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(now_us, deliver, lost);
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
        awaiting_keyframe_ = false;
        loss_reported_ = false;
        last_delivered_ = seq;
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(now_us, deliver, lost);
        return;
    }

    // Delta frame behind a gap: hold it until the gap closes or expires.
    const int64_t key = key_for(seq);
    if (held_.find(key) != held_.end()) return;   // duplicate of a held frame
    if (held_.empty()) gap_since_us_ = now_us;      // the wait for last+1 starts now
    held_bytes_ += frame.size();
    held_.emplace(key, Held{hdr, std::move(frame), now_us});

    if (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        require_keyframe(now_us, lost);
        bound_held();
    } else if (next_is_aborted()) {
        require_keyframe(now_us, lost);
    }
}

void VideoFrameReorderBuffer::on_frame_aborted(uint32_t frame_seq, int64_t now_us,
                                               const DeliverFn& /*deliver*/, const LostFn& lost) {
    // The keyframe we were counting on is gone: whoever waits on it must ask again.
    const bool lost_inflight_key = inflight_key_ && frame_seq == inflight_key_seq_;
    if (lost_inflight_key) inflight_key_ = false;

    if (!have_last_) return;   // the owner's keyframe retry covers startup

    if (frame_seq == last_delivered_ + 1) {
        // No successor can be decoded without a fresh random-access point.
        require_keyframe(now_us, lost);
        if (lost_inflight_key) report_loss(lost);
        return;
    }

    if (!video_seq_newer(frame_seq, last_delivered_)) return;   // already behind us

    if (lost_inflight_key && awaiting_keyframe_) report_loss(lost);

    // A held frame cannot be aborted (it completed), but be tolerant.
    auto it = held_.find(key_for(frame_seq));
    if (it != held_.end()) {
        held_bytes_ -= it->second.frame.size();
        held_.erase(it);
        gap_since_us_ = earliest_arrival(held_);
        return;
    }
    // Ahead of the chain: remember it, so reaching it fails at once instead
    // of waiting out the hold.
    if (std::find(aborted_.begin(), aborted_.end(), frame_seq) == aborted_.end()) {
        if (aborted_.size() >= MAX_ABORTED_TRACKED) aborted_.erase(aborted_.begin());
        aborted_.push_back(frame_seq);
    }
}

void VideoFrameReorderBuffer::poll(int64_t now_us, const DeliverFn& /*deliver*/, const LostFn& lost) {
    const int64_t hold = hold_us();
    if (!have_last_) {
        for (auto it = held_.begin(); it != held_.end();) {
            if (now_us - it->second.arrived_us >= hold) {
                held_bytes_ -= it->second.frame.size();
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        gap_since_us_ = earliest_arrival(held_);
        return;
    }
    if (awaiting_keyframe_) {
        // The keyframe we stayed quiet for never completed: ask now. Held
        // deltas stay: a recovery keyframe older than them resumes from them
        // (dropping them would break the chain right after that keyframe).
        if (!loss_reported_ && !keyframe_in_flight(now_us)) report_loss(lost);
        return;
    }
    if (!held_.empty() && now_us - gap_since_us_ >= hold)
        require_keyframe(now_us, lost);
}

void VideoFrameReorderBuffer::deliver_one(Held&& h, const DeliverFn& deliver) {
    forget_inflight_through(h.hdr.frame_seq);
    if (deliver) deliver(h.hdr, std::move(h.frame));
}

void VideoFrameReorderBuffer::drain(int64_t now_us, const DeliverFn& deliver, const LostFn& lost) {
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
    std::erase_if(aborted_, [this](uint32_t seq) { return !video_seq_newer(seq, last_delivered_); });
    if (next_is_aborted()) require_keyframe(now_us, lost);
}

void VideoFrameReorderBuffer::require_keyframe(int64_t now_us, const LostFn& lost) {
    if (awaiting_keyframe_) return;
    awaiting_keyframe_ = true;
    loss_reported_ = false;
    // A newer keyframe is already arriving: it repairs this gap by itself.
    if (!keyframe_in_flight(now_us)) report_loss(lost);
}

void VideoFrameReorderBuffer::report_loss(const LostFn& lost) {
    if (loss_reported_ || !awaiting_keyframe_) return;
    loss_reported_ = true;
    if (lost) lost(last_delivered_ + 1);
}

void VideoFrameReorderBuffer::bound_held() {
    while (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        // Before the first keyframe the newest deltas are the useful ones;
        // after it the oldest are, since a late retransmission resumes from them.
        auto victim = have_last_ ? std::prev(held_.end()) : held_.begin();
        held_bytes_ -= victim->second.frame.size();
        held_.erase(victim);
    }
    gap_since_us_ = earliest_arrival(held_);
}

} // namespace parties
