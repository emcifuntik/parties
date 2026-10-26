#pragma once

#include <parties/audio_common.h>

#include <speex/speex_echo.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace parties::client {

class EchoCanceller {
public:
    static constexpr size_t PLAYBACK_CHANNELS = 2;
    static constexpr size_t REFERENCE_DELAY_FRAMES = 2;
    static constexpr size_t QUEUE_CAPACITY_FRAMES = 64;

    EchoCanceller() = default;
    ~EchoCanceller();

    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    bool init();
    void shutdown();

    void set_enabled(bool enabled);
    bool is_enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void request_reset();

    void push_playback(const float* stereo, size_t frame_count);

    bool process_capture(float* mono, size_t frame_count);

private:
    static constexpr size_t QUEUE_SAMPLE_CAPACITY =
        QUEUE_CAPACITY_FRAMES * static_cast<size_t>(audio::FRAME_SIZE);

    void rebase_capture_timing(uint64_t epoch);
    void publish_playback_frame();

    SpeexEchoState* state_ = nullptr;

    std::unique_ptr<std::atomic<uint32_t>[]> reference_queue_;
    std::array<uint32_t, audio::FRAME_SIZE> playback_frame_{};
    size_t playback_frame_pos_ = 0;

    std::atomic<uint64_t> written_frames_{0};
    uint64_t read_frame_ = 0;

    std::atomic<uint64_t> reset_epoch_{1};
    uint64_t playback_epoch_ = 0;
    uint64_t capture_epoch_ = 0;
    std::atomic<uint64_t> playback_seen_epoch_{0};
    std::atomic<uint64_t> epoch_first_frame_{0};

    size_t delay_frames_remaining_ = 0;
    bool capture_synchronized_ = false;
    bool reference_starved_ = true;
    std::atomic<bool> enabled_{false};
};

}
