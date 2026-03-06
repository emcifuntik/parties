#pragma once

#include <parties/codec.h>
#include <parties/audio_common.h>

#include <miniaudio.h>
#include <rnnoise.h>
#include <speex/speex_echo.h>

#include <functional>
#include <atomic>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

namespace parties::client {

class VoiceMixer;

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

    // Called when an encoded voice frame is ready to send
    std::function<void(const uint8_t*, size_t)> on_encoded_frame;

    // Mute/unmute
    void set_muted(bool muted) { muted_ = muted; }
    bool is_muted() const { return muted_; }

    // Deafen
    void set_deafened(bool deafened) { deafened_ = deafened; }
    bool is_deafened() const { return deafened_; }

    // Device enumeration
    std::vector<DeviceInfo> get_capture_devices() const;
    std::vector<DeviceInfo> get_playback_devices() const;
    void set_capture_device(int index);
    void set_playback_device(int index);

    // Denoise toggle
    void set_denoise_enabled(bool enabled) { denoise_enabled_ = enabled; }
    bool is_denoise_enabled() const { return denoise_enabled_; }

    // Voice normalization (playback)
    void set_normalize_enabled(bool enabled) { normalize_enabled_ = enabled; }
    void set_normalize_target(float target) { normalize_target_ = target; }

    // Echo cancellation
    void set_aec_enabled(bool enabled) { aec_enabled_ = enabled; }
    bool is_aec_enabled() const { return aec_enabled_; }

    // Voice activation detection
    void set_vad_enabled(bool enabled) { vad_enabled_ = enabled; }
    void set_vad_threshold(float threshold) { vad_threshold_ = threshold; }

    // Current mic input level (0..1, updated per frame)
    float voice_level() const { return voice_level_; }

private:
    static void data_callback(ma_device* device, void* output,
                               const void* input, ma_uint32 frame_count);

    void process_capture(const float* input, ma_uint32 frame_count);
    void process_playback(float* output, ma_uint32 frame_count);

    bool init_device();

    // miniaudio context (owns device enumeration)
    ma_context context_{};
    bool context_initialized_ = false;

    ma_device device_{};
    bool device_initialized_ = false;
    bool running_ = false;

    // Device selection (-1 = system default)
    int selected_capture_ = -1;
    int selected_playback_ = -1;
    std::vector<ma_device_id> capture_ids_;
    std::vector<ma_device_id> playback_ids_;
    std::vector<std::string> capture_names_;
    std::vector<std::string> playback_names_;

    // RNNoise denoiser
    DenoiseState* rnn_ = nullptr;

    // SpeexDSP echo canceller
    SpeexEchoState* aec_ = nullptr;
    std::vector<spx_int16_t> aec_ref_buf_;     // playback reference ring buffer
    size_t aec_ref_write_ = 0;
    size_t aec_ref_read_ = 0;

    // Opus encoder
    OpusCodec encoder_;

    // Capture accumulation buffer
    std::vector<float> capture_buf_;
    size_t capture_pos_ = 0;

    // Encode accumulation buffer (2 RNNoise frames -> 20ms Opus)
    float encode_buf_[audio::OPUS_FRAME_SIZE];
    size_t encode_pos_ = 0;

    uint8_t opus_buf_[audio::MAX_OPUS_PACKET];

    VoiceMixer* mixer_ = nullptr;

    std::atomic<bool> muted_{false};
    std::atomic<bool> deafened_{false};
    std::atomic<bool> denoise_enabled_{true};
    std::atomic<bool> aec_enabled_{false};
    std::atomic<bool> normalize_enabled_{false};
    std::atomic<float> normalize_target_{0.5f};
    std::atomic<bool> vad_enabled_{false};
    std::atomic<float> vad_threshold_{0.02f};
    std::atomic<float> voice_level_{0.0f};

    // Normalization gain smoothing (used in audio callback)
    float current_gain_ = 1.0f;

    // VAD hold timer (frames to keep transmitting after voice drops below threshold)
    int vad_hold_frames_ = 0;
    static constexpr int VAD_HOLD_COUNT = 30; // ~300ms at 10ms frames
};

} // namespace parties::client
