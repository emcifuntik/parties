#include <parties/video_receive_router.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

// Viewer-side routing of per-frame video streams. See the header.
//
// This translation unit is also compiled stand-alone by the router test, so
// it must not depend on the logging or tracing facilities of parties_common.

namespace parties {

namespace {

int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A stream whose queue grows past this plays its oldest frames at once (the
// clock is off, e.g. the sender's capture clock jumped).
constexpr size_t MAX_PENDING_FRAMES = 64;

} // namespace

VideoReceiveRouter::VideoReceiveRouter(DeliverFn deliver, SendRequestFn send_request,
                                       const VideoReceiveRouterConfig& cfg)
    : cfg_(cfg), deliver_(std::move(deliver)), send_request_(std::move(send_request)) {
    if (cfg_.base_backoff_ms == 0) cfg_.base_backoff_ms = 1;
    if (cfg_.max_backoff_ms < cfg_.base_backoff_ms) cfg_.max_backoff_ms = cfg_.base_backoff_ms;
    if (cfg_.playout) playout_thread_ = std::thread([this] { playout_loop(); });
}

VideoReceiveRouter::~VideoReceiveRouter() {
    {
        std::lock_guard<std::mutex> lock(playout_mutex_);
        playout_stop_ = true;
    }
    playout_cv_.notify_all();
    if (playout_thread_.joinable()) playout_thread_.join();
}

void VideoReceiveRouter::playout_loop() {
    std::unique_lock<std::mutex> lock(playout_mutex_);
    std::vector<std::pair<uint32_t, Pending>> due;
    while (!playout_stop_) {
        const int64_t now = steady_us();
        int64_t earliest = (std::numeric_limits<int64_t>::max)();
        for (auto& [stream, queue] : pending_) {
            while (!queue.empty() && (queue.front().at_us <= now || queue.size() > MAX_PENDING_FRAMES)) {
                due.emplace_back(stream, std::move(queue.front()));
                queue.pop_front();
            }
            if (!queue.empty()) earliest = (std::min)(earliest, queue.front().at_us);
        }
        if (!due.empty()) {
            lock.unlock();
            for (auto& [stream, p] : due)
                if (deliver_) deliver_(stream, p.hdr, std::move(p.frame));
            due.clear();
            lock.lock();
            continue;
        }
        if (earliest == (std::numeric_limits<int64_t>::max)())
            playout_cv_.wait(lock);
        else
            playout_cv_.wait_for(lock, std::chrono::microseconds(earliest - now));
    }
}

void VideoReceiveRouter::open(uint32_t stream, int64_t now_us) {
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        StreamState state{VideoFrameReorderBuffer(cfg_.reorder), VideoPlayoutClock(cfg_.playout_clock), {}};
        state.buffer.set_rtt(rtt_us_);
        streams_.insert_or_assign(stream, std::move(state));
    }
    std::lock_guard<std::mutex> lock(pacing_mutex_);
    pacing_.erase(stream);
    pacing_stats_.erase(stream);
    if (now_us != 0) {
        Pacing& p = pacing_[stream];
        p.backoff_us = static_cast<int64_t>(cfg_.base_backoff_ms) * 1000;
        p.next_us = now_us + p.backoff_us;
    }
}

void VideoReceiveRouter::close(uint32_t stream) {
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        streams_.erase(stream);
    }
    {
        std::lock_guard<std::mutex> lock(playout_mutex_);
        pending_.erase(stream);
    }
    std::lock_guard<std::mutex> lock(pacing_mutex_);
    pacing_.erase(stream);
    pacing_stats_.erase(stream);
}

void VideoReceiveRouter::close_all() {
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        streams_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(playout_mutex_);
        pending_.clear();
    }
    std::lock_guard<std::mutex> lock(pacing_mutex_);
    pacing_.clear();
    pacing_stats_.clear();
}

bool VideoReceiveRouter::is_open(uint32_t stream) const {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    return streams_.find(stream) != streams_.end();
}

void VideoReceiveRouter::set_rtt(uint32_t rtt_us) {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    rtt_us_ = rtt_us;
    for (auto& [id, state] : streams_) {
        (void)id;
        state.buffer.set_rtt(rtt_us);
    }
}

VideoFrameReorderBuffer::DeliverFn VideoReceiveRouter::deliver_fn(uint32_t stream, StreamState& state,
                                                                  int64_t now_us) {
    return [this, stream, &state, now_us](const VideoFrameHeader& hdr, std::vector<uint8_t>&& frame) {
        ++state.stats.frames_delivered;
        if (hdr.keyframe()) {
            ++state.stats.keyframes_delivered;
            keyframe_arrived(stream);
        }
        if (!cfg_.playout) {
            if (deliver_) deliver_(stream, hdr, std::move(frame));
            return;
        }
        const int64_t at = state.clock.playout_time(hdr, now_us);
        state.stats.playout_delay_us = state.clock.delay_us();
        {
            std::lock_guard<std::mutex> lock(playout_mutex_);
            pending_[stream].push_back(Pending{at, hdr, std::move(frame)});
        }
        playout_cv_.notify_one();
    };
}

VideoFrameReorderBuffer::LostFn VideoReceiveRouter::lost_fn(uint32_t stream, int64_t now_us) {
    return [this, stream, now_us](uint32_t) { request_keyframe(stream, now_us); };
}

void VideoReceiveRouter::on_frame_started(uint32_t stream, const VideoFrameHeader& hdr, int64_t now_us) {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) return;
    it->second.buffer.on_frame_started(hdr, now_us);
}

void VideoReceiveRouter::on_frame(uint32_t stream, std::vector<uint8_t>&& frame, int64_t now_us) {
    VideoFrameHeader hdr;
    if (!VideoFrameHeader::parse(frame.data(), frame.size(), hdr)) return;
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) return;   // not watching (any more): drop
    it->second.buffer.on_frame(hdr, std::move(frame), now_us,
                               deliver_fn(stream, it->second, now_us), lost_fn(stream, now_us));
}

void VideoReceiveRouter::on_frame_aborted(uint32_t stream, uint32_t frame_seq, bool have_seq,
                                          int64_t now_us) {
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        auto it = streams_.find(stream);
        if (it == streams_.end()) return;
        if (have_seq) {
            it->second.buffer.on_frame_aborted(frame_seq, now_us, deliver_fn(stream, it->second, now_us),
                                               lost_fn(stream, now_us));
            return;
        }
        // Which frame died is unknown: unless a keyframe is already arriving,
        // only a fresh one is certain to repair it.
        if (it->second.buffer.keyframe_in_flight(now_us)) return;
    }
    request_keyframe(stream, now_us);
}

void VideoReceiveRouter::request_keyframe(uint32_t stream, int64_t now_us) {
    bool send = false;
    {
        std::lock_guard<std::mutex> lock(pacing_mutex_);
        Pacing& p = pacing_[stream];
        Stats& s = pacing_stats_[stream];
        if (p.backoff_us == 0) p.backoff_us = static_cast<int64_t>(cfg_.base_backoff_ms) * 1000;
        if (now_us < p.next_us) {
            ++s.requests_paced;
        } else {
            p.next_us = now_us + p.backoff_us;
            p.backoff_us = (std::min)(p.backoff_us * 2, static_cast<int64_t>(cfg_.max_backoff_ms) * 1000);
            ++s.requests_sent;
            send = true;
        }
    }
    if (send && send_request_) send_request_(stream);
}

void VideoReceiveRouter::keyframe_arrived(uint32_t stream) {
    std::lock_guard<std::mutex> lock(pacing_mutex_);
    auto it = pacing_.find(stream);
    if (it == pacing_.end()) return;
    it->second.next_us = 0;
    it->second.backoff_us = static_cast<int64_t>(cfg_.base_backoff_ms) * 1000;
}

void VideoReceiveRouter::tick(int64_t now_us, const std::function<bool(uint32_t)>& decoder_needs_keyframe) {
    struct Candidate { uint32_t stream; bool wants; bool key_arriving; };
    std::vector<Candidate> candidates;
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        if (streams_.empty()) return;
        candidates.reserve(streams_.size());
        for (auto& [id, state] : streams_) {
            state.buffer.poll(now_us, deliver_fn(id, state, now_us), lost_fn(id, now_us));
            candidates.push_back({id, state.buffer.should_request_keyframe(now_us),
                                  state.buffer.keyframe_in_flight(now_us)});
        }
    }
    // The decoder predicate runs without our locks: it may take the owner's.
    for (const auto& c : candidates) {
        const bool wants = c.wants ||
            (!c.key_arriving && decoder_needs_keyframe && decoder_needs_keyframe(c.stream));
        if (wants) request_keyframe(c.stream, now_us);
    }
}

bool VideoReceiveRouter::should_request_keyframe(uint32_t stream, int64_t now_us) const {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    auto it = streams_.find(stream);
    return it != streams_.end() && it->second.buffer.should_request_keyframe(now_us);
}

VideoReceiveRouter::Stats VideoReceiveRouter::stats(uint32_t stream) const {
    Stats out;
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        auto it = streams_.find(stream);
        if (it != streams_.end()) out = it->second.stats;
    }
    std::lock_guard<std::mutex> lock(pacing_mutex_);
    auto it = pacing_stats_.find(stream);
    if (it != pacing_stats_.end()) {
        out.requests_sent = it->second.requests_sent;
        out.requests_paced = it->second.requests_paced;
    }
    return out;
}

} // namespace parties
