#include <parties/video_playout.h>

#include <algorithm>
#include <vector>

// Playout clock. See the header. Also compiled stand-alone by the router
// test: no logging or tracing dependencies.

namespace parties {

void VideoPlayoutClock::reset() {
    have_ts_ = false;
    last_ts_ = 0;
    last_ext_ms_ = 0;
    delay_us_ = 0;
    last_update_us_ = 0;
    samples_.clear();
}

int64_t VideoPlayoutClock::playout_time(const VideoFrameHeader& hdr, int64_t arrival_us) {
    if ((hdr.flags & VIDEO_FLAG_CAPTURE_TIME) == 0) return arrival_us;

    // Unwrap the 32-bit millisecond capture clock.
    if (!have_ts_) {
        have_ts_ = true;
        last_ext_ms_ = hdr.timestamp;
    } else {
        last_ext_ms_ += static_cast<int32_t>(hdr.timestamp - last_ts_);
    }
    last_ts_ = hdr.timestamp;
    const int64_t capture_us = last_ext_ms_ * 1000;
    const int64_t transit = arrival_us - capture_us;

    const int64_t window_us = static_cast<int64_t>(cfg_.window_ms) * 1000;
    samples_.push_back({arrival_us, transit});
    while (!samples_.empty() && arrival_us - samples_.front().at_us > window_us)
        samples_.pop_front();

    int64_t base = transit;
    for (const auto& s : samples_) base = std::min(base, s.transit_us);
    std::vector<int64_t> excess;
    excess.reserve(samples_.size());
    for (const auto& s : samples_) excess.push_back(s.transit_us - base);
    const size_t k = static_cast<size_t>(cfg_.percentile * static_cast<double>(excess.size() - 1));
    std::nth_element(excess.begin(), excess.begin() + static_cast<ptrdiff_t>(k), excess.end());
    const int64_t max_delay = static_cast<int64_t>(cfg_.max_delay_ms) * 1000;
    const int64_t target = std::min(excess[k], max_delay);

    // Up at once (a late frame now would stall), down slowly (a quiet spell
    // must not take the cushion away right before the next burst).
    if (target >= delay_us_) {
        delay_us_ = target;
    } else if (last_update_us_ != 0) {
        const int64_t step = (arrival_us - last_update_us_) * cfg_.decay_ms_per_s / 1000;
        delay_us_ = std::max(target, delay_us_ - step);
    }
    last_update_us_ = arrival_us;

    const int64_t at = capture_us + base + delay_us_;
    return std::clamp(at, arrival_us, arrival_us + max_delay);
}

} // namespace parties
