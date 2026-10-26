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
        const int64_t k = video_seq_distance(h.frame.header().frame_seq, base);
        rebased.emplace(k, std::move(h));
    }
    held.swap(rebased);
}

template <class Map>
std::optional<int64_t> earliest_arrival(const Map& held) {
    std::optional<int64_t> earliest;
    for (const auto& [key, h] : held) {
        (void)key;
        if (!earliest || h.arrived_us < *earliest) earliest = h.arrived_us;
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
    gap_since_us_.reset();
}

int64_t VideoFrameReorderBuffer::key_for(uint32_t seq) const {
    return static_cast<int64_t>(video_seq_distance(seq, last_delivered_));
}

ReorderOutcome VideoFrameReorderBuffer::on_frame(ParsedVideoFrame&& frame, int64_t now_us) {
    ReorderOutcome out;
    const uint32_t seq = frame.header().frame_seq;
    const bool keyframe = frame.header().keyframe;

    if (have_last_ && !video_seq_newer(seq, last_delivered_)) return out;

    if (!have_last_) {
        if (!keyframe) {
            if (held_.empty()) last_delivered_ = seq;
            const int64_t key = key_for(seq);
            if (held_.contains(key)) return out;
            held_bytes_ += frame.wire_size();
            held_.emplace(key, Held{std::move(frame), now_us});
            bound_held();
            return out;
        }
        for (auto it = held_.begin(); it != held_.end();) {
            if (!video_seq_newer(it->second.frame.header().frame_seq, seq)) {
                held_bytes_ -= it->second.frame.wire_size();
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        have_last_ = true;
        awaiting_keyframe_ = false;
        last_delivered_ = seq;
        rekey_held(held_, seq);
        out.deliver.push_back(std::move(frame));
        drain(out);
        return out;
    }

    if (seq == last_delivered_ + 1) {
        awaiting_keyframe_ = false;
        last_delivered_ = seq;
        out.deliver.push_back(std::move(frame));
        drain(out);
        return out;
    }

    if (keyframe) {
        const int64_t key = key_for(seq);
        auto end = held_.upper_bound(key);
        for (auto it = held_.begin(); it != end; ++it)
            held_bytes_ -= it->second.frame.wire_size();
        held_.erase(held_.begin(), end);
        awaiting_keyframe_ = false;
        last_delivered_ = seq;
        out.deliver.push_back(std::move(frame));
        drain(out);
        return out;
    }

    const int64_t key = key_for(seq);
    if (held_.find(key) != held_.end()) return out;
    if (held_.empty()) gap_since_us_ = now_us;
    held_bytes_ += frame.wire_size();
    held_.emplace(key, Held{std::move(frame), now_us});

    if (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        out.keyframe = require_keyframe();
        bound_held();
    }
    return out;
}

KeyframeRequest VideoFrameReorderBuffer::on_frame_aborted(uint32_t frame_seq) {
    if (!have_last_) return KeyframeRequest::None;

    if (frame_seq == last_delivered_ + 1) return require_keyframe();

    if (!video_seq_newer(frame_seq, last_delivered_)) return KeyframeRequest::None;

    auto it = held_.find(key_for(frame_seq));
    if (it == held_.end()) return KeyframeRequest::None;
    held_bytes_ -= it->second.frame.wire_size();
    held_.erase(it);
    gap_since_us_ = earliest_arrival(held_);
    return KeyframeRequest::None;
}

KeyframeRequest VideoFrameReorderBuffer::poll(int64_t now_us) {
    const int64_t max_hold_us = static_cast<int64_t>(cfg_.max_hold_ms) * 1000;
    if (!have_last_) {
        for (auto it = held_.begin(); it != held_.end();) {
            if (now_us - it->second.arrived_us >= max_hold_us) {
                held_bytes_ -= it->second.frame.wire_size();
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        gap_since_us_ = earliest_arrival(held_);
        return KeyframeRequest::None;
    }
    if (awaiting_keyframe_) return KeyframeRequest::None;
    if (gap_since_us_ && now_us - *gap_since_us_ >= max_hold_us)
        return require_keyframe();
    return KeyframeRequest::None;
}

void VideoFrameReorderBuffer::drain(ReorderOutcome& out) {
    while (!held_.empty()) {
        auto it = held_.begin();
        if (it->second.frame.header().frame_seq != last_delivered_ + 1) break;
        Held h = std::move(it->second);
        held_.erase(it);
        held_bytes_ -= h.frame.wire_size();
        last_delivered_ = h.frame.header().frame_seq;
        out.deliver.push_back(std::move(h.frame));
    }
    rekey_held(held_, last_delivered_);
    gap_since_us_ = earliest_arrival(held_);
}

KeyframeRequest VideoFrameReorderBuffer::require_keyframe() {
    if (awaiting_keyframe_) return KeyframeRequest::None;
    awaiting_keyframe_ = true;
    return KeyframeRequest::Needed;
}

void VideoFrameReorderBuffer::bound_held() {
    while (held_.size() > cfg_.max_held_frames || held_bytes_ > cfg_.max_bytes) {
        held_bytes_ -= held_.begin()->second.frame.wire_size();
        held_.erase(held_.begin());
    }
    gap_since_us_ = earliest_arrival(held_);
}

}
