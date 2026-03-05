#pragma once

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace parties::client {

class SoundPlayer {
public:
    enum class Effect {
        Mute,
        Unmute,
        Deafen,
        Undeafen,
        JoinChannel,
        LeaveChannel,
        UserJoined,
        UserLeft,
        Count_
    };

    SoundPlayer();
    ~SoundPlayer();

    // Start/stop the dedicated playback device
    bool init();
    void shutdown();

    // Queue a sound to play (call from any thread)
    void play(Effect effect);

private:
    static constexpr int kSampleRate = 48000;
    static constexpr int kMaxPlaying = 8;

    static void data_callback(ma_device* device, void* output,
                               const void* input, ma_uint32 frame_count);

    struct Sound {
        std::vector<float> samples;
    };

    std::array<Sound, static_cast<size_t>(Effect::Count_)> sounds_;

    struct PlayingSound {
        std::atomic<int> effect{-1};     // -1 = inactive
        std::atomic<size_t> position{0};
    };

    std::array<PlayingSound, kMaxPlaying> playing_;

    ma_device device_{};
    bool device_initialized_ = false;
};

} // namespace parties::client
