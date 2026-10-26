#pragma once

#include <parties/codec.h>
#include <parties/audio_common.h>
#include <client/echo_canceller.h>
#include <client/audio_activity.h>

#include <miniaudio.h>
#include <rnnoise.h>

#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

namespace parties::client {

class VoiceMixer;
class StreamAudioPlayer;

struct DeviceInfo {
    std::string name;
    int index = 0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();

    bool start();
    void stop();

    void set_mixer(VoiceMixer* mixer) { mixer_ = mixer; }
    void set_aux_mixer(VoiceMixer* mixer) { aux_mixer_ = mixer; }
    void set_stream_player(StreamAudioPlayer* player) { stream_player_ = player; }

    std::function<void(const uint8_t*, size_t)> on_encoded_frame;

    std::function<void(const uint8_t*, size_t)> on_secondary_encoded_frame;

    void push_secondary_pcm(const float* pcm, int frame_count);
    void set_secondary_send_volume(float volume);
    bool is_secondary_transmitting() const { return secondary_activity_.active(); }
    float secondary_send_volume() const {
        return secondary_send_volume_.load(std::memory_order_relaxed);
    }

    void set_muted(bool muted) { muted_ = muted; if (muted) transmitting_ = false; }
    bool is_muted() const { return muted_; }

    void set_deafened(bool deafened) { deafened_ = deafened; }
    bool is_deafened() const { return deafened_; }

    std::vector<DeviceInfo> get_capture_devices() const;
    std::vector<DeviceInfo> get_playback_devices() const;
    int default_capture_index() const { return default_capture_; }
    int default_playback_index() const { return default_playback_; }
    int selected_capture_index() const {
        return selected_capture_ >= 0 ? selected_capture_ : default_capture_;
    }
    int selected_playback_index() const {
        return selected_playback_ >= 0 ? selected_playback_ : default_playback_;
    }
    bool set_capture_device(int index);
    bool set_playback_device(int index);

    void set_denoise_enabled(bool enabled) { denoise_enabled_ = enabled; }
    bool is_denoise_enabled() const { return denoise_enabled_; }

    void set_normalize_enabled(bool enabled) { normalize_enabled_ = enabled; }
    void set_normalize_target(float target) { normalize_target_ = target; }

    void set_aec_enabled(bool enabled) { echo_canceller_.set_enabled(enabled); }
    bool is_aec_enabled() const { return echo_canceller_.is_enabled(); }

    void set_vad_enabled(bool enabled) { vad_enabled_ = enabled; }
    void set_vad_threshold(float threshold) { vad_threshold_ = threshold; }

    float voice_level() const { return voice_level_; }

    bool is_transmitting() const { return transmitting_; }

private:
    static void capture_callback(ma_device* device, void* output,
                                  const void* input, ma_uint32 frame_count);
    static void playback_callback(ma_device* device, void* output,
                                   const void* input, ma_uint32 frame_count);

    void process_capture(const float* input, ma_uint32 frame_count);
    void process_playback(float* output, ma_uint32 frame_count);

    bool init_devices();
    void uninit_devices();
    bool reconfigure_devices(int capture_index, int playback_index);

    ma_context context_{};
    bool context_initialized_ = false;

    ma_device capture_device_{};
    ma_device playback_device_{};
    bool capture_initialized_ = false;
    bool playback_initialized_ = false;
    bool running_ = false;

    int selected_capture_ = -1;
    int selected_playback_ = -1;
    int default_capture_ = 0;
    int default_playback_ = 0;
    std::vector<ma_device_id> capture_ids_;
    std::vector<ma_device_id> playback_ids_;
    std::vector<std::string> capture_names_;
    std::vector<std::string> playback_names_;

    DenoiseState* rnn_ = nullptr;

    EchoCanceller echo_canceller_;

    OpusCodec encoder_;

    OpusCodec secondary_encoder_;
    bool secondary_encoder_ready_ = false;
    std::mutex secondary_mutex_;
    std::vector<float> secondary_buf_;
    size_t secondary_pos_ = 0;
    uint8_t secondary_opus_buf_[audio::MAX_OPUS_PACKET];
    std::atomic<float> secondary_send_volume_{1.0f};
    AudioActivity secondary_activity_;

    std::vector<float> capture_buf_;
    size_t capture_pos_ = 0;

    float encode_buf_[audio::OPUS_FRAME_SIZE];
    size_t encode_pos_ = 0;

    uint8_t opus_buf_[audio::MAX_OPUS_PACKET];

    VoiceMixer* mixer_ = nullptr;
    VoiceMixer* aux_mixer_ = nullptr;
    StreamAudioPlayer* stream_player_ = nullptr;

    std::atomic<bool> muted_{false};
    std::atomic<bool> deafened_{false};
    std::atomic<bool> denoise_enabled_{true};
    std::atomic<bool> normalize_enabled_{false};
    std::atomic<float> normalize_target_{0.8f};
    std::atomic<bool> vad_enabled_{true};
    std::atomic<float> vad_threshold_{0.43f};
    std::atomic<float> voice_level_{0.0f};
    std::atomic<bool>  transmitting_{false};

    float current_gain_ = 1.0f;

    int vad_hold_frames_ = 0;
    static constexpr int VAD_HOLD_COUNT = 30;
};

}
