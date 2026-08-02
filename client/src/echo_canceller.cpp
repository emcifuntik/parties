#include <client/echo_canceller.h>

#include <algorithm>
#include <bit>
#include <limits>

namespace parties::client {
namespace {

spx_int16_t float_to_s16(float sample) {
    sample = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<spx_int16_t>(sample * 32767.0f);
}

uint32_t pack_stereo(spx_int16_t left, spx_int16_t right) {
    const auto left_bits = std::bit_cast<uint16_t>(left);
    const auto right_bits = std::bit_cast<uint16_t>(right);
    return static_cast<uint32_t>(left_bits) |
           (static_cast<uint32_t>(right_bits) << 16u);
}

void unpack_stereo(uint32_t packed, spx_int16_t& left, spx_int16_t& right) {
    left = std::bit_cast<spx_int16_t>(static_cast<uint16_t>(packed));
    right = std::bit_cast<spx_int16_t>(static_cast<uint16_t>(packed >> 16u));
}

} // namespace

EchoCanceller::~EchoCanceller() {
    shutdown();
}

bool EchoCanceller::init() {
    if (state_)
        return true;

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "AEC reference transport must remain lock-free");

    // One microphone and two independent speaker references. A mono downmix is
    // insufficient for stereo streams because each speaker has a different
    // acoustic path to the microphone.
    state_ = speex_echo_state_init_mc(audio::FRAME_SIZE,
                                      audio::SAMPLE_RATE / 5,
                                      1,
                                      static_cast<int>(PLAYBACK_CHANNELS));
    if (!state_)
        return false;

    int rate = audio::SAMPLE_RATE;
    speex_echo_ctl(state_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    reference_queue_ = std::make_unique<std::atomic<uint32_t>[]>(QUEUE_SAMPLE_CAPACITY);
    for (size_t i = 0; i < QUEUE_SAMPLE_CAPACITY; ++i)
        reference_queue_[i].store(0, std::memory_order_relaxed);

    written_frames_.store(0, std::memory_order_relaxed);
    playback_seen_epoch_.store(0, std::memory_order_relaxed);
    epoch_first_frame_.store(0, std::memory_order_relaxed);
    playback_frame_pos_ = 0;
    playback_epoch_ = 0;
    capture_epoch_ = 0;
    capture_synchronized_ = false;
    delay_frames_remaining_ = 0;
    reference_starved_ = true;
    return true;
}

void EchoCanceller::shutdown() {
    enabled_.store(false, std::memory_order_relaxed);
    if (state_) {
        speex_echo_state_destroy(state_);
        state_ = nullptr;
    }
    reference_queue_.reset();
}

void EchoCanceller::set_enabled(bool enabled) {
    if (enabled_.exchange(enabled, std::memory_order_acq_rel) != enabled)
        request_reset();
}

void EchoCanceller::request_reset() {
    reset_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void EchoCanceller::publish_playback_frame() {
    const uint64_t sequence = written_frames_.load(std::memory_order_relaxed);
    const size_t base = static_cast<size_t>(sequence % QUEUE_CAPACITY_FRAMES) *
                        static_cast<size_t>(audio::FRAME_SIZE);
    for (size_t i = 0; i < static_cast<size_t>(audio::FRAME_SIZE); ++i)
        reference_queue_[base + i].store(playback_frame_[i], std::memory_order_relaxed);

    // Publish only after every packed stereo sample is visible to capture.
    written_frames_.store(sequence + 1, std::memory_order_release);
}

void EchoCanceller::push_playback(const float* stereo, size_t frame_count) {
    if (!stereo || !state_ || !reference_queue_ ||
        !enabled_.load(std::memory_order_acquire))
        return;

    const uint64_t epoch = reset_epoch_.load(std::memory_order_acquire);
    if (playback_epoch_ != epoch) {
        playback_epoch_ = epoch;
        playback_frame_pos_ = 0;
        epoch_first_frame_.store(written_frames_.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
        playback_seen_epoch_.store(epoch, std::memory_order_release);
    }

    for (size_t i = 0; i < frame_count; ++i) {
        const spx_int16_t left = float_to_s16(stereo[i * PLAYBACK_CHANNELS]);
        const spx_int16_t right = float_to_s16(stereo[i * PLAYBACK_CHANNELS + 1]);
        playback_frame_[playback_frame_pos_++] = pack_stereo(left, right);

        if (playback_frame_pos_ == static_cast<size_t>(audio::FRAME_SIZE)) {
            publish_playback_frame();
            playback_frame_pos_ = 0;
        }
    }
}

void EchoCanceller::rebase_capture_timing(uint64_t epoch) {
    capture_epoch_ = epoch;
    capture_synchronized_ = false;
    delay_frames_remaining_ = 0;
    reference_starved_ = true;
}

bool EchoCanceller::process_capture(float* mono, size_t frame_count) {
    if (!mono || frame_count != static_cast<size_t>(audio::FRAME_SIZE) ||
        !state_ || !reference_queue_ ||
        !enabled_.load(std::memory_order_acquire))
        return false;

    const uint64_t epoch = reset_epoch_.load(std::memory_order_acquire);
    if (capture_epoch_ != epoch)
        rebase_capture_timing(epoch);

    if (playback_seen_epoch_.load(std::memory_order_acquire) != epoch)
        return false;

    if (!capture_synchronized_) {
        read_frame_ = epoch_first_frame_.load(std::memory_order_relaxed);
        capture_synchronized_ = true;
    }

    const uint64_t written = written_frames_.load(std::memory_order_acquire);
    if (written <= read_frame_) {
        if (!reference_starved_) {
            // An xrun or clock discontinuity broke the capture/reference
            // relationship. Wait for fresh playback and establish delay again.
            reference_starved_ = true;
            delay_frames_remaining_ = 0;
        }
        return false;
    }

    if (reference_starved_) {
        reference_starved_ = false;
        delay_frames_remaining_ = REFERENCE_DELAY_FRAMES;
    }

    // Playback callbacks render ahead of what has physically reached the
    // speakers. Hold capture for two 10 ms frames before pairing it with the
    // first queued reference so Speex never receives an impossible future
    // reference. Its adaptive filter handles the remaining device/acoustic lag.
    if (delay_frames_remaining_ > 0) {
        --delay_frames_remaining_;
        return false;
    }

    // Capture was paused long enough for playback to overwrite unread data.
    // Drop the stale queue and restart alignment instead of feeding a torn or
    // unrelated reference into the adaptive filter.
    if (written - read_frame_ >= QUEUE_CAPACITY_FRAMES) {
        read_frame_ = written;
        delay_frames_remaining_ = 0;
        reference_starved_ = true;
        return false;
    }

    spx_int16_t microphone[audio::FRAME_SIZE];
    spx_int16_t reference[audio::FRAME_SIZE * PLAYBACK_CHANNELS];
    spx_int16_t cancelled[audio::FRAME_SIZE];

    const size_t base = static_cast<size_t>(read_frame_ % QUEUE_CAPACITY_FRAMES) *
                        static_cast<size_t>(audio::FRAME_SIZE);
    for (size_t i = 0; i < static_cast<size_t>(audio::FRAME_SIZE); ++i) {
        microphone[i] = float_to_s16(mono[i]);
        const uint32_t packed = reference_queue_[base + i].load(std::memory_order_relaxed);
        unpack_stereo(packed,
                      reference[i * PLAYBACK_CHANNELS],
                      reference[i * PLAYBACK_CHANNELS + 1]);
    }

    // If playback lapped capture during the copy, do not process a mixed slot.
    const uint64_t written_after = written_frames_.load(std::memory_order_acquire);
    if (written_after - read_frame_ >= QUEUE_CAPACITY_FRAMES) {
        read_frame_ = written_after;
        delay_frames_remaining_ = 0;
        reference_starved_ = true;
        return false;
    }
    ++read_frame_;

    speex_echo_cancellation(state_, microphone, reference, cancelled);

    constexpr float S16_TO_FLOAT = 1.0f / 32767.0f;
    for (size_t i = 0; i < static_cast<size_t>(audio::FRAME_SIZE); ++i)
        mono[i] = static_cast<float>(cancelled[i]) * S16_TO_FLOAT;
    return true;
}

} // namespace parties::client
