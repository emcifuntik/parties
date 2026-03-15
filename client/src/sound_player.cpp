#include <client/sound_player.h>

#include <parties/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace parties::client {

// Embedded WAV files (16-bit PCM, 48kHz, exported from sounds/generator.html)
static constexpr unsigned char wav_unmute[] = {
    #embed "../../sounds/parties-unmute.wav"
};
static constexpr unsigned char wav_mute[] = {
    #embed "../../sounds/parties-mute.wav"
};
static constexpr unsigned char wav_undeafen[] = {
    #embed "../../sounds/parties-undeafen.wav"
};
static constexpr unsigned char wav_deafen[] = {
    #embed "../../sounds/parties-deafen.wav"
};
static constexpr unsigned char wav_join_self[] = {
    #embed "../../sounds/parties-join-self.wav"
};
static constexpr unsigned char wav_leave_self[] = {
    #embed "../../sounds/parties-leave-self.wav"
};
static constexpr unsigned char wav_join_other[] = {
    #embed "../../sounds/parties-join-other.wav"
};
static constexpr unsigned char wav_leave_other[] = {
    #embed "../../sounds/parties-leave-other.wav"
};

// Decode 16-bit PCM WAV to float samples.
// Scans for "data" subchunk to handle varying header sizes.
static std::vector<float> decode_wav(const unsigned char* data, size_t size) {
    std::vector<float> out;
    // Find "data" subchunk
    for (size_t i = 0; i + 8 < size; i++) {
        if (data[i] == 'd' && data[i+1] == 'a' &&
            data[i+2] == 't' && data[i+3] == 'a') {
            uint32_t chunk_size;
            std::memcpy(&chunk_size, &data[i + 4], 4);
            size_t pcm_start = i + 8;
            size_t pcm_bytes = std::min(static_cast<size_t>(chunk_size),
                                        size - pcm_start);
            size_t num_samples = pcm_bytes / 2;
            out.resize(num_samples);
            for (size_t s = 0; s < num_samples; s++) {
                int16_t sample;
                std::memcpy(&sample, &data[pcm_start + s * 2], 2);
                out[s] = static_cast<float>(sample) / 32768.0f;
            }
            break;
        }
    }
    return out;
}

SoundPlayer::SoundPlayer() {
    for (auto& p : playing_)
        p.effect.store(-1, std::memory_order_relaxed);

    sounds_[static_cast<size_t>(Effect::Unmute)].samples =
        decode_wav(wav_unmute, sizeof(wav_unmute));
    sounds_[static_cast<size_t>(Effect::Mute)].samples =
        decode_wav(wav_mute, sizeof(wav_mute));
    sounds_[static_cast<size_t>(Effect::Undeafen)].samples =
        decode_wav(wav_undeafen, sizeof(wav_undeafen));
    sounds_[static_cast<size_t>(Effect::Deafen)].samples =
        decode_wav(wav_deafen, sizeof(wav_deafen));
    sounds_[static_cast<size_t>(Effect::JoinChannel)].samples =
        decode_wav(wav_join_self, sizeof(wav_join_self));
    sounds_[static_cast<size_t>(Effect::LeaveChannel)].samples =
        decode_wav(wav_leave_self, sizeof(wav_leave_self));
    sounds_[static_cast<size_t>(Effect::UserJoined)].samples =
        decode_wav(wav_join_other, sizeof(wav_join_other));
    sounds_[static_cast<size_t>(Effect::UserLeft)].samples =
        decode_wav(wav_leave_other, sizeof(wav_leave_other));
}

SoundPlayer::~SoundPlayer() {
    shutdown();
}

bool SoundPlayer::init() {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = kSampleRate;
    config.dataCallback = SoundPlayer::data_callback;
    config.pUserData = this;
    config.periodSizeInMilliseconds = 10;

    if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to init playback device");
        return false;
    }
    device_initialized_ = true;

    LOG_INFO("SoundPlayer device: {}", device_.playback.name);
    LOG_INFO("SoundPlayer requested: {} Hz, native: {} Hz, channels: {}",
             kSampleRate,
             device_.playback.internalSampleRate,
             device_.playback.internalChannels);

    if (ma_device_start(&device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to start playback device");
        ma_device_uninit(&device_);
        device_initialized_ = false;
        return false;
    }

    return true;
}

void SoundPlayer::shutdown() {
    if (device_initialized_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialized_ = false;
    }
}

void SoundPlayer::play(Effect effect) {
    int idx = static_cast<int>(effect);
    if (idx < 0 || idx >= static_cast<int>(Effect::Count_)) return;

    for (auto& slot : playing_) {
        int expected = -1;
        if (slot.effect.compare_exchange_strong(expected, idx,
                std::memory_order_acq_rel)) {
            slot.position.store(0, std::memory_order_release);
            return;
        }
    }
}

void SoundPlayer::data_callback(ma_device* device, void* output,
                                  const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<SoundPlayer*>(device->pUserData);
    auto* out = static_cast<float*>(output);
    std::memset(out, 0, frame_count * sizeof(float));

    float vol = self->volume_.load(std::memory_order_relaxed);

    for (auto& slot : self->playing_) {
        int fx = slot.effect.load(std::memory_order_acquire);
        if (fx < 0) continue;

        auto& pcm = self->sounds_[fx].samples;
        size_t pos = slot.position.load(std::memory_order_acquire);
        size_t remaining = pcm.size() - pos;
        size_t to_mix = std::min(static_cast<size_t>(frame_count), remaining);

        for (size_t i = 0; i < to_mix; i++)
            out[i] += pcm[pos + i] * vol;

        pos += to_mix;
        if (pos >= pcm.size()) {
            slot.effect.store(-1, std::memory_order_release);
        } else {
            slot.position.store(pos, std::memory_order_release);
        }
    }
}

} // namespace parties::client
