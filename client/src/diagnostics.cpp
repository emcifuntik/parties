#include <client/diagnostics.h>

#include <algorithm>

// Diagnostics history and freeze classification. See the header. Also
// compiled stand-alone by tests/diagnostics_test.cpp: no RmlUi or logging.

namespace parties::client {

namespace {

constexpr int64_t SECOND_US = 1'000'000;
constexpr int64_t MINUTE_US = 60 * SECOND_US;

// A delivery gap whose capture times jumped by at least this share of it was
// a gap at the sender (no frames produced), not a delay on the way.
constexpr double SENDER_GAP_SHARE = 0.6;

} // namespace

const char* DiagnosticsRecorder::cause_name(FreezeCause cause) {
    switch (cause) {
    case FreezeCause::Sender:  return "sender";
    case FreezeCause::Network: return "network";
    case FreezeCause::Lost:    return "lost frames";
    case FreezeCause::Decode:  return "decode";
    case FreezeCause::Render:  return "render";
    }
    return "?";
}

void DiagnosticsRecorder::push(History& h, float v) {
    std::move(h.begin() + 1, h.end(), h.begin());
    h.back() = v;
}

void DiagnosticsRecorder::trim(std::deque<int64_t>& q, int64_t now_us) {
    while (!q.empty() && now_us - q.front() > SECOND_US) q.pop_front();
}

void DiagnosticsRecorder::record_freeze(Freeze f) {
    f.id = next_freeze_id_++;
    freezes_.push_back(f);
    while (freezes_.size() > MAX_FREEZES) freezes_.pop_front();
}

void DiagnosticsRecorder::on_frame_delivered(uint32_t stream, const VideoFrameHeader& hdr,
                                             size_t bytes, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamState& st = streams_[stream];
    ++st.frames_delivered;
    if (hdr.keyframe()) ++st.keyframes;
    st.bucket_bytes += bytes;
    st.delivered_times.push_back(now_us);
    trim(st.delivered_times, now_us);
    st.delivered_bytes.emplace_back(now_us, bytes);
    while (!st.delivered_bytes.empty() && now_us - st.delivered_bytes.front().first > SECOND_US)
        st.delivered_bytes.pop_front();

    FrameMark mark;
    mark.at_us = now_us;
    mark.frame_seq = hdr.frame_seq;
    if (hdr.flags & VIDEO_FLAG_CAPTURE_TIME) {
        if (!st.have_delivery || !st.last_delivery.has_capture)
            st.capture_ext_ms = hdr.timestamp;
        else
            st.capture_ext_ms += static_cast<int32_t>(hdr.timestamp - st.last_capture_raw);
        st.last_capture_raw = hdr.timestamp;
        mark.capture_ms = st.capture_ext_ms;
        mark.has_capture = true;
    }

    if (st.have_delivery) {
        const FrameMark& prev = st.last_delivery;
        const int64_t gap = now_us - prev.at_us;
        // Negative: the sequence restarted (new share); not a loss.
        const int32_t skipped = static_cast<int32_t>(hdr.frame_seq - prev.frame_seq) - 1;
        if (skipped > 0) {
            st.frames_lost += static_cast<uint32_t>(skipped);
            st.lost_since_present += static_cast<uint32_t>(skipped);
        }
        if (gap >= FREEZE_US / 2) {
            DeliveryGap& g = st.delivery_gap;
            g.end_us = now_us;
            g.duration_us = gap;
            if (skipped > 0)
                g.cause = FreezeCause::Lost;
            else if (mark.has_capture && prev.has_capture &&
                     static_cast<double>(mark.capture_ms - prev.capture_ms) * 1000.0 >=
                         SENDER_GAP_SHARE * static_cast<double>(gap))
                g.cause = FreezeCause::Sender;
            else
                g.cause = FreezeCause::Network;
            g.recorded = gap >= (g.cause == FreezeCause::Sender ? SENDER_FREEZE_US : FREEZE_US);
            if (g.recorded) {
                Freeze f;
                f.at_us = now_us;
                f.duration_us = gap;
                f.stream = stream;
                f.cause = g.cause;
                if (skipped > 0) f.frames_lost = static_cast<uint32_t>(skipped);
                st.bucket_freeze = true;
                record_freeze(f);
            }
        }
    }
    st.last_delivery = mark;
    st.have_delivery = true;
}

void DiagnosticsRecorder::on_frame_presented(uint32_t stream, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamState& st = streams_[stream];
    ++st.frames_presented;
    ++st.bucket_presented;
    st.presented_times.push_back(now_us);
    trim(st.presented_times, now_us);
    if (st.last_present_us != 0 && now_us - st.last_present_us >= FREEZE_US) {
        const int64_t gap = now_us - st.last_present_us;
        // A stall that ended inside this gap and covers most of it explains it.
        auto covers = [&](int64_t end_us, int64_t duration_us) {
            return end_us >= st.last_present_us && duration_us * 2 >= gap;
        };
        const DeliveryGap& g = st.delivery_gap;
        const bool by_delivery = covers(g.end_us, g.duration_us);
        Freeze f;
        f.at_us = now_us;
        f.duration_us = gap;
        f.stream = stream;
        bool record = true;
        if (covers(render_stall_end_us_, render_stall_us_)) {
            record = false;                       // recorded once, for the app
        } else if (by_delivery && (g.recorded || g.cause == FreezeCause::Sender)) {
            record = false;                       // recorded with its cause, or a short static pause
        } else if (st.lost_since_present > 0) {
            // Frames went missing (perhaps without a long delivery gap) and
            // the decoder waited for the keyframe that resumed playback.
            f.cause = FreezeCause::Lost;
            f.frames_lost = st.lost_since_present;
        } else {
            f.cause = by_delivery ? g.cause : FreezeCause::Decode;
        }
        st.bucket_freeze = true;
        if (record) record_freeze(f);
    }
    st.last_present_us = now_us;
    st.lost_since_present = 0;
}

void DiagnosticsRecorder::on_render_tick(int64_t now_us, bool drawing) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!drawing) {
        // Nothing can be presented while paused: restart every stream's
        // presentation clock instead of reporting the pause as a freeze.
        render_paused_ = true;
        for (auto& [id, st] : streams_) st.last_present_us = 0;
        return;
    }
    const int64_t prev = last_render_us_;
    const bool resumed = render_paused_;
    last_render_us_ = now_us;
    render_paused_ = false;
    if (prev == 0 || resumed || now_us - prev < FREEZE_US) return;
    render_stall_end_us_ = now_us;
    render_stall_us_ = now_us - prev;
    Freeze f;
    f.at_us = now_us;
    f.duration_us = render_stall_us_;
    f.stream = APP_STREAM;
    f.cause = FreezeCause::Render;
    record_freeze(f);
}

void DiagnosticsRecorder::forget_stream(uint32_t stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(stream);
}

void DiagnosticsRecorder::sample(int64_t now_us, const DiagConnectionSample& conn,
                                 const DiagServerReport& server) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server.valid) server_ = server;
    if (bucket_start_us_ == 0) {
        bucket_start_us_ = now_us;
        last_conn_ = conn;
        have_conn_ = conn.valid;
        return;
    }
    const int64_t elapsed = now_us - bucket_start_us_;
    if (elapsed < BUCKET_US) return;
    const double dt = static_cast<double>(elapsed) / 1e6;

    auto& s = series_;
    auto at = [&s](Series x) -> History& { return s[static_cast<size_t>(x)]; };
    if (conn.valid && have_conn_ && conn.recv_bytes >= last_conn_.recv_bytes &&
        conn.sent_bytes >= last_conn_.sent_bytes) {
        push(at(Series::DownKbps), static_cast<float>((conn.recv_bytes - last_conn_.recv_bytes) * 8 / dt / 1000.0));
        push(at(Series::UpKbps), static_cast<float>((conn.sent_bytes - last_conn_.sent_bytes) * 8 / dt / 1000.0));
        const uint64_t sent = conn.sent_packets - last_conn_.sent_packets;
        const uint64_t lost = conn.lost_packets >= last_conn_.lost_packets
            ? conn.lost_packets - last_conn_.lost_packets : 0;
        push(at(Series::UpLossPct), sent ? static_cast<float>(100.0 * static_cast<double>(lost) /
                                                              static_cast<double>(sent)) : 0.0f);
        push(at(Series::RttMs), static_cast<float>(conn.rtt_us) / 1000.0f);
    } else {
        push(at(Series::DownKbps), 0);
        push(at(Series::UpKbps), 0);
        push(at(Series::UpLossPct), 0);
        push(at(Series::RttMs), conn.valid ? static_cast<float>(conn.rtt_us) / 1000.0f : 0.0f);
    }
    push(at(Series::DownLossPct), server_.valid ? static_cast<float>(server_.loss_permille) / 10.0f : 0.0f);
    push(at(Series::ServerRttMs), server_.valid ? static_cast<float>(server_.srtt_us) / 1000.0f : 0.0f);

    for (auto& [id, st] : streams_) {
        push(st.fps, static_cast<float>(st.bucket_presented / dt));
        const bool stalled = st.last_present_us != 0 && now_us - st.last_present_us >= FREEZE_US;
        push(st.freeze, (st.bucket_freeze || stalled) ? 1.0f : 0.0f);
        st.bucket_presented = 0;
        st.bucket_bytes = 0;
        st.bucket_freeze = false;
    }

    bucket_start_us_ = now_us;
    last_conn_ = conn;
    have_conn_ = conn.valid;
    ++revision_;
}

DiagnosticsRecorder::History DiagnosticsRecorder::history(Series series) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return series_[static_cast<size_t>(series)];
}

DiagnosticsRecorder::History DiagnosticsRecorder::stream_fps(uint32_t stream) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream);
    return it == streams_.end() ? History{} : it->second.fps;
}

DiagnosticsRecorder::History DiagnosticsRecorder::stream_freezes(uint32_t stream) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream);
    return it == streams_.end() ? History{} : it->second.freeze;
}

std::vector<DiagnosticsRecorder::StreamSummary> DiagnosticsRecorder::streams(int64_t now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamSummary> out;
    out.reserve(streams_.size());
    auto in_last_second = [now_us](const std::deque<int64_t>& q) {
        return static_cast<double>(std::count_if(q.begin(), q.end(),
                                                 [now_us](int64_t t) { return now_us - t <= SECOND_US; }));
    };
    for (const auto& [id, st] : streams_) {
        StreamSummary s;
        s.stream = id;
        s.fps_presented = in_last_second(st.presented_times);
        s.fps_delivered = in_last_second(st.delivered_times);
        uint64_t bytes = 0;
        for (const auto& [t, b] : st.delivered_bytes)
            if (now_us - t <= SECOND_US) bytes += b;
        s.recv_kbps = static_cast<double>(bytes) * 8.0 / 1000.0;
        s.frames_delivered = st.frames_delivered;
        s.frames_presented = st.frames_presented;
        s.frames_lost = st.frames_lost;
        s.keyframes = st.keyframes;
        for (const auto& f : freezes_) {
            if (f.stream != id || now_us - f.at_us > MINUTE_US) continue;
            ++s.freezes_minute;
            s.longest_freeze_us = (std::max)(s.longest_freeze_us, f.duration_us);
        }
        if (st.last_present_us != 0 && now_us - st.last_present_us >= FREEZE_US)
            s.stalled_us = now_us - st.last_present_us;
        out.push_back(s);
    }
    return out;
}

std::vector<DiagnosticsRecorder::Freeze> DiagnosticsRecorder::freezes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {freezes_.begin(), freezes_.end()};
}

uint64_t DiagnosticsRecorder::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

DiagServerReport DiagnosticsRecorder::server_report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_;
}

DiagConnectionSample DiagnosticsRecorder::last_connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_conn_;
}

} // namespace parties::client
