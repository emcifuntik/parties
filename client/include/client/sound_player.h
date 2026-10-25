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
        ServerConnected,
        ServerDisconnected,
        StreamStarted,
        ViewerJoined,
        Count_
    };

    SoundPlayer();
    ~SoundPlayer();

    bool init();
    void shutdown();

    void play(Effect effect);

    void set_volume(float v) { volume_.store(v, std::memory_order_relaxed); }
    float get_volume() const { return volume_.load(std::memory_order_relaxed); }

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
        std::atomic<int> effect{-1};
        std::atomic<size_t> position{0};
    };

    std::array<PlayingSound, kMaxPlaying> playing_;
    std::atomic<float> volume_{1.0f};

    ma_context context_{};
    bool context_initialized_ = false;
    ma_device device_{};
    bool device_initialized_ = false;
};

}
