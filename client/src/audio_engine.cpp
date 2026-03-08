#include <client/audio_engine.h>
#include <parties/profiler.h>
#include <client/voice_mixer.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace parties::client {

AudioEngine::AudioEngine()
    : capture_buf_(audio::FRAME_SIZE, 0.0f) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
	ZoneScopedN("AudioEngine::init");
    // Initialize miniaudio context (needed for device enumeration)
    if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
        std::fprintf(stderr, "[Audio] Failed to initialize audio context\n");
        return false;
    }
    context_initialized_ = true;

    // Enumerate devices
    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 capture_count = 0;
    ma_context_get_devices(&context_, &playback_infos, &playback_count,
                           &capture_infos, &capture_count);

    capture_ids_.clear();
    capture_names_.clear();
    for (ma_uint32 i = 0; i < capture_count; i++) {
        capture_ids_.push_back(capture_infos[i].id);
        capture_names_.push_back(capture_infos[i].name);
    }

    playback_ids_.clear();
    playback_names_.clear();
    for (ma_uint32 i = 0; i < playback_count; i++) {
        playback_ids_.push_back(playback_infos[i].id);
        playback_names_.push_back(playback_infos[i].name);
    }

    // Initialize RNNoise
    rnn_ = rnnoise_create(nullptr);
    if (!rnn_) {
        std::fprintf(stderr, "[Audio] Failed to create RNNoise state\n");
        return false;
    }

    // Initialize SpeexDSP echo canceller
    // filter_length = 100ms worth of samples at 48kHz = 4800
    aec_ = speex_echo_state_init(audio::FRAME_SIZE, audio::SAMPLE_RATE / 10);
    if (!aec_) {
        std::fprintf(stderr, "[Audio] Failed to create Speex AEC state\n");
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
        return false;
    }
    {
        int rate = audio::SAMPLE_RATE;
        speex_echo_ctl(aec_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }
    // Ring buffer for playback reference: enough for ~50ms latency headroom
    aec_ref_buf_.resize(audio::SAMPLE_RATE / 10, 0);
    aec_ref_write_ = 0;
    aec_ref_read_ = 0;

    // Initialize Opus encoder
    if (!encoder_.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS, audio::OPUS_BITRATE)) {
        std::fprintf(stderr, "[Audio] Failed to create Opus encoder\n");
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
        return false;
    }

    if (!init_devices()) return false;

    return true;
}

bool AudioEngine::init_devices() {
    // Separate capture and playback devices to avoid duplex resampling issues
    // (virtual surround headsets with different native sample rates cause artifacts in duplex mode)

    // --- Capture device ---
    ma_device_config cap_config = ma_device_config_init(ma_device_type_capture);
    cap_config.capture.format = ma_format_f32;
    cap_config.capture.channels = audio::CHANNELS;
    cap_config.sampleRate = audio::SAMPLE_RATE;
    cap_config.dataCallback = AudioEngine::capture_callback;
    cap_config.pUserData = this;
    cap_config.periodSizeInMilliseconds = 10;

    if (selected_capture_ >= 0 && selected_capture_ < static_cast<int>(capture_ids_.size()))
        cap_config.capture.pDeviceID = &capture_ids_[selected_capture_];

    if (ma_device_init(&context_, &cap_config, &capture_device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[Audio] Failed to initialize capture device\n");
        return false;
    }
    capture_initialized_ = true;

    // --- Playback device ---
    ma_device_config play_config = ma_device_config_init(ma_device_type_playback);
    play_config.playback.format = ma_format_f32;
    play_config.playback.channels = audio::CHANNELS;
    play_config.sampleRate = audio::SAMPLE_RATE;
    play_config.dataCallback = AudioEngine::playback_callback;
    play_config.pUserData = this;
    play_config.periodSizeInMilliseconds = 10;

    if (selected_playback_ >= 0 && selected_playback_ < static_cast<int>(playback_ids_.size()))
        play_config.playback.pDeviceID = &playback_ids_[selected_playback_];

    if (ma_device_init(&context_, &play_config, &playback_device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[Audio] Failed to initialize playback device\n");
        ma_device_uninit(&capture_device_);
        capture_initialized_ = false;
        return false;
    }
    playback_initialized_ = true;

    capture_pos_ = 0;
    encode_pos_ = 0;

    std::printf("[Audio] Capture:  %s (%d Hz, %dch, native=%d Hz %dch)\n",
                capture_device_.capture.name,
                capture_device_.sampleRate,
                capture_device_.capture.channels,
                capture_device_.capture.internalSampleRate,
                capture_device_.capture.internalChannels);
    std::printf("[Audio] Playback: %s (%d Hz, %dch, native=%d Hz %dch)\n",
                playback_device_.playback.name,
                playback_device_.sampleRate,
                playback_device_.playback.channels,
                playback_device_.playback.internalSampleRate,
                playback_device_.playback.internalChannels);
    return true;
}

void AudioEngine::shutdown() {
    stop();
    if (playback_initialized_) {
        ma_device_uninit(&playback_device_);
        playback_initialized_ = false;
    }
    if (capture_initialized_) {
        ma_device_uninit(&capture_device_);
        capture_initialized_ = false;
    }
    if (aec_) {
        speex_echo_state_destroy(aec_);
        aec_ = nullptr;
    }
    if (rnn_) {
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
    }
    if (context_initialized_) {
        ma_context_uninit(&context_);
        context_initialized_ = false;
    }
}

bool AudioEngine::start() {
    if (!capture_initialized_ || !playback_initialized_ || running_) return false;
    if (ma_device_start(&playback_device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[Audio] Failed to start playback device\n");
        return false;
    }
    if (ma_device_start(&capture_device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[Audio] Failed to start capture device\n");
        ma_device_stop(&playback_device_);
        return false;
    }
    running_ = true;
    std::printf("[Audio] Started\n");
    return true;
}

void AudioEngine::stop() {
    if (!running_) return;
    ma_device_stop(&capture_device_);
    ma_device_stop(&playback_device_);
    running_ = false;
    std::printf("[Audio] Stopped\n");
}

std::vector<DeviceInfo> AudioEngine::get_capture_devices() const {
    std::vector<DeviceInfo> result;
    for (size_t i = 0; i < capture_names_.size(); i++)
        result.push_back({capture_names_[i], static_cast<int>(i)});
    return result;
}

std::vector<DeviceInfo> AudioEngine::get_playback_devices() const {
    std::vector<DeviceInfo> result;
    for (size_t i = 0; i < playback_names_.size(); i++)
        result.push_back({playback_names_[i], static_cast<int>(i)});
    return result;
}

void AudioEngine::set_capture_device(int index) {
    if (index == selected_capture_) return;
    selected_capture_ = index;

    bool was_running = running_;
    stop();
    if (capture_initialized_) {
        ma_device_uninit(&capture_device_);
        capture_initialized_ = false;
    }
    if (playback_initialized_) {
        ma_device_uninit(&playback_device_);
        playback_initialized_ = false;
    }
    if (init_devices() && was_running)
        start();
}

void AudioEngine::set_playback_device(int index) {
    if (index == selected_playback_) return;
    selected_playback_ = index;

    bool was_running = running_;
    stop();
    if (capture_initialized_) {
        ma_device_uninit(&capture_device_);
        capture_initialized_ = false;
    }
    if (playback_initialized_) {
        ma_device_uninit(&playback_device_);
        playback_initialized_ = false;
    }
    if (init_devices() && was_running)
        start();
}

void AudioEngine::capture_callback(ma_device* device, void* /*output*/,
                                    const void* input, ma_uint32 frame_count) {
    auto* engine = static_cast<AudioEngine*>(device->pUserData);
    engine->process_capture(static_cast<const float*>(input), frame_count);
}

void AudioEngine::playback_callback(ma_device* device, void* output,
                                     const void* /*input*/, ma_uint32 frame_count) {
    auto* engine = static_cast<AudioEngine*>(device->pUserData);
    engine->process_playback(static_cast<float*>(output), frame_count);

    // Store playback reference for AEC (extract mono from stereo, float→int16 into ring buffer)
    if (engine->aec_enabled_ && engine->aec_) {
        auto* out_f = static_cast<const float*>(output);
        size_t cap = engine->aec_ref_buf_.size();
        for (ma_uint32 i = 0; i < frame_count; i++) {
            float s = out_f[i];
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            engine->aec_ref_buf_[engine->aec_ref_write_ % cap] =
                static_cast<spx_int16_t>(s * 32767.0f);
            engine->aec_ref_write_++;
        }
    }
}

void AudioEngine::process_capture(const float* input, ma_uint32 frame_count) {
	ZoneScopedN("AudioEngine::process_capture");
    if (muted_ || !on_encoded_frame) return;

    ma_uint32 remaining = frame_count;
    const float* src = input;

    while (remaining > 0) {
        size_t space = audio::FRAME_SIZE - capture_pos_;
        size_t to_copy = std::min(static_cast<size_t>(remaining), space);

        std::memcpy(capture_buf_.data() + capture_pos_, src, to_copy * sizeof(float));
        capture_pos_ += to_copy;
        src += to_copy;
        remaining -= static_cast<ma_uint32>(to_copy);

        if (capture_pos_ >= static_cast<size_t>(audio::FRAME_SIZE)) {
            // AEC: cancel echo from capture using playback reference
            if (aec_enabled_ && aec_) {
                size_t cap = aec_ref_buf_.size();
                size_t avail = aec_ref_write_ - aec_ref_read_;
                if (avail >= static_cast<size_t>(audio::FRAME_SIZE)) {
                    spx_int16_t mic[audio::FRAME_SIZE];
                    spx_int16_t ref[audio::FRAME_SIZE];
                    spx_int16_t out[audio::FRAME_SIZE];

                    for (int i = 0; i < audio::FRAME_SIZE; i++) {
                        float s = capture_buf_[i];
                        if (s > 1.0f) s = 1.0f;
                        else if (s < -1.0f) s = -1.0f;
                        mic[i] = static_cast<spx_int16_t>(s * 32767.0f);
                        ref[i] = aec_ref_buf_[(aec_ref_read_ + i) % cap];
                    }
                    aec_ref_read_ += audio::FRAME_SIZE;

                    speex_echo_cancellation(aec_, mic, ref, out);

                    for (int i = 0; i < audio::FRAME_SIZE; i++)
                        capture_buf_[i] = static_cast<float>(out[i]) / 32767.0f;
                }
            }

            float processed[audio::FRAME_SIZE];

            if (denoise_enabled_) {
                // RNNoise: scale to int16 range, denoise, scale back
                float rnn_in[audio::FRAME_SIZE];
                float rnn_out[audio::FRAME_SIZE];
                for (int i = 0; i < audio::FRAME_SIZE; i++)
                    rnn_in[i] = capture_buf_[i] * 32768.0f;

                rnnoise_process_frame(rnn_, rnn_out, rnn_in);

                for (int i = 0; i < audio::FRAME_SIZE; i++)
                    processed[i] = rnn_out[i] / 32768.0f;
            } else {
                // Pass through raw audio
                std::memcpy(processed, capture_buf_.data(),
                           audio::FRAME_SIZE * sizeof(float));
            }

            // Compute RMS for voice level meter
            float sum = 0.0f;
            for (int i = 0; i < audio::FRAME_SIZE; i++)
                sum += processed[i] * processed[i];
            float rms = std::sqrt(sum / audio::FRAME_SIZE);
            voice_level_ = rms;

            // VAD check
            if (vad_enabled_) {
                float threshold = vad_threshold_.load();
                if (rms >= threshold) {
                    vad_hold_frames_ = VAD_HOLD_COUNT;
                } else if (vad_hold_frames_ > 0) {
                    vad_hold_frames_--;
                } else {
                    // Below threshold and hold expired — don't encode
                    capture_pos_ = 0;
                    continue;
                }
            }

            // Append to encode buffer
            for (int i = 0; i < audio::FRAME_SIZE; i++)
                encode_buf_[encode_pos_ + i] = processed[i];
            encode_pos_ += audio::FRAME_SIZE;

            // When we have 2 RNNoise frames (960 samples = 20ms), Opus encode
            if (encode_pos_ >= static_cast<size_t>(audio::OPUS_FRAME_SIZE)) {
                int encoded = encoder_.encode(encode_buf_, audio::OPUS_FRAME_SIZE,
                                               opus_buf_, audio::MAX_OPUS_PACKET);
                if (encoded > 0)
                    on_encoded_frame(opus_buf_, static_cast<size_t>(encoded));
                encode_pos_ = 0;
            }

            capture_pos_ = 0;
        }
    }
}

void AudioEngine::process_playback(float* output, ma_uint32 frame_count) {
	ZoneScopedN("AudioEngine::process_playback");
    std::memset(output, 0, frame_count * sizeof(float));

    // Mix voice audio (skip when deafened)
    if (!deafened_ && mixer_)
        mixer_->mix_output(output, static_cast<int>(frame_count));

    // Voice normalization
    if (normalize_enabled_ && !deafened_) {
        float sum = 0.0f;
        for (ma_uint32 i = 0; i < frame_count; i++)
            sum += output[i] * output[i];
        float rms = std::sqrt(sum / frame_count);

        if (rms > 0.001f) {
            float target = normalize_target_.load();
            float desired_gain = target / rms;
            desired_gain = std::clamp(desired_gain, 0.1f, 10.0f);

            // Exponential smoothing to avoid clicks
            float alpha = 0.05f;
            current_gain_ += alpha * (desired_gain - current_gain_);

            for (ma_uint32 i = 0; i < frame_count; i++) {
                output[i] *= current_gain_;
                // Soft clip
                if (output[i] > 1.0f) output[i] = 1.0f;
                else if (output[i] < -1.0f) output[i] = -1.0f;
            }
        }
    }
}

} // namespace parties::client
