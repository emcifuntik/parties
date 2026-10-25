#include <parties/video_frame_reorder.h>

#include <algorithm>
#include <utility>

namespace parties {

namespace {

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

}

VideoFrameReorderBuffer::VideoFrameReorderBuffer()
    : VideoFrameReorderBuffer(VideoFrameReorderConfig{}) {}

VideoFrameReorderBuffer::VideoFrameReorderBuffer(const VideoFrameReorderConfig& cfg)
    : cfg_(cfg) {
    if (cfg_.max_held_frames == 0) cfg_.max_held_frames = 1;
}

void VideoFrameReorderBuffer::reset() {
    have_last_ = false;
    awaiting_keyframe_ = true;
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

    if (have_last_ && !video_seq_newer(seq, last_delivered_)) return;

    if (!have_last_ || awaiting_keyframe_) {
        if (!hdr.keyframe()) {
            if (!have_last_ && held_.empty()) last_delivered_ = seq;
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
        last_delivered_ = seq;
        rekey_held(held_, seq);
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(deliver);
        return;
    }

    if (seq == last_delivered_ + 1) {
        last_delivered_ = seq;
        deliver_one(Held{hdr, std::move(frame), now_us}, deliver);
        drain(deliver);
        return;
    }

    if (hdr.keyframe()) {
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

    const int64_t key = key_for(seq);
    if (held_.find(key) != held_.end()) return;
    if (held_.empty()) gap_since_us_ = now_us;
    held_bytes_ += frame.size();
    held_.emplace(key, Held{hdr, std::move(frame), now_us});

    if (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        require_keyframe(lost);
        bound_held();
    }
}

void VideoFrameReorderBuffer::on_frame_aborted(uint32_t frame_seq, int64_t,
                                               const DeliverFn&, const LostFn& lost) {
    if (!have_last_) return;

    if (frame_seq == last_delivered_ + 1) {
        require_keyframe(lost);
        return;
    }

    if (!video_seq_newer(frame_seq, last_delivered_)) return;

    auto it = held_.find(key_for(frame_seq));
    if (it == held_.end()) return;
    held_bytes_ -= it->second.frame.size();
    held_.erase(it);
    gap_since_us_ = earliest_arrival(held_);
}

void VideoFrameReorderBuffer::poll(int64_t now_us, const DeliverFn&, const LostFn& lost) {
    const int64_t max_hold_us = static_cast<int64_t>(cfg_.max_hold_ms) * 1000;
    if (!have_last_ || awaiting_keyframe_) {
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
    if (!held_.empty() && now_us - gap_since_us_ >= max_hold_us)
        require_keyframe(lost);
}

void VideoFrameReorderBuffer::deliver_one(Held&& h, const DeliverFn& deliver) {
    if (deliver) deliver(h.hdr, std::move(h.frame));
}

void VideoFrameReorderBuffer::drain(const DeliverFn& deliver) {
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

void VideoFrameReorderBuffer::require_keyframe(const LostFn& lost) {
    if (awaiting_keyframe_) return;
    awaiting_keyframe_ = true;
    lost(last_delivered_ + 1);
}

void VideoFrameReorderBuffer::bound_held() {
    while (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        held_bytes_ -= held_.begin()->second.frame.size();
        held_.erase(held_.begin());
    }
    gap_since_us_ = earliest_arrival(held_);
}

}
