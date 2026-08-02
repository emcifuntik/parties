#include <client/audio_engine.h>
#include <client/stream_audio_player.h>
#include <parties/profiler.h>
#include <client/voice_mixer.h>

#include <parties/log.h>

#include <cstring>
#include <algorithm>
#include <cmath>

namespace parties::client {

static void capture_notification(const ma_device_notification* pNotification) {
    if (pNotification->type == ma_device_notification_type_started)
        TracySetThreadName("AudioCapture");
}

static void playback_notification(const ma_device_notification* pNotification) {
    if (pNotification->type == ma_device_notification_type_started)
        TracySetThreadName("AudioPlayback");
}

AudioEngine::AudioEngine()
    : capture_buf_(audio::FRAME_SIZE, 0.0f) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
	ZoneScopedN("AudioEngine::init");
    // Initialize miniaudio context (needed for device enumeration)
    if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize audio context");
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
    default_capture_ = 0;
    for (ma_uint32 i = 0; i < capture_count; i++) {
        capture_ids_.push_back(capture_infos[i].id);
        capture_names_.push_back(capture_infos[i].name);
        if (capture_infos[i].isDefault)
            default_capture_ = static_cast<int>(i);
    }

    playback_ids_.clear();
    playback_names_.clear();
    default_playback_ = 0;
    for (ma_uint32 i = 0; i < playback_count; i++) {
        playback_ids_.push_back(playback_infos[i].id);
        playback_names_.push_back(playback_infos[i].name);
        if (playback_infos[i].isDefault)
            default_playback_ = static_cast<int>(i);
    }

    // Initialize RNNoise
    rnn_ = rnnoise_create(nullptr);
    if (!rnn_) {
        LOG_ERROR("Failed to create RNNoise state");
        return false;
    }

    if (!echo_canceller_.init()) {
        LOG_ERROR("Failed to initialize stereo echo cancellation");
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
        return false;
    }

    // Initialize Opus encoder with in-band FEC for packet-loss resilience.
    if (!encoder_.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS, audio::OPUS_BITRATE,
                               /*inband_fec=*/true, audio::OPUS_EXPECTED_LOSS_PCT)) {
        LOG_ERROR("Failed to create Opus encoder");
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
        return false;
    }

    // Secondary/auxiliary stream encoder (music profile, mono). Optional — if it
    // fails we just don't offer a 2nd stream; the primary path is unaffected.
    secondary_buf_.assign(audio::OPUS_FRAME_SIZE, 0.0f);
    secondary_pos_ = 0;
    secondary_encoder_ready_ =
        secondary_encoder_.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS,
                                        audio::SECONDARY_OPUS_BITRATE,
                                        /*inband_fec=*/true, audio::OPUS_EXPECTED_LOSS_PCT,
                                        OpusMode::Music);
    if (!secondary_encoder_ready_)
        LOG_WARN("Secondary audio encoder unavailable; 2nd stream disabled");

    if (!init_devices()) return false;

    return true;
}

void AudioEngine::push_secondary_pcm(const float* pcm, int frame_count) {
    if (!pcm || frame_count <= 0 || !on_secondary_encoded_frame) return;

    std::lock_guard<std::mutex> lock(secondary_mutex_);
    if (!secondary_encoder_ready_) return;

    const float gain = audio::volume_position_to_gain(
        secondary_send_volume_.load(std::memory_order_relaxed));
    int remaining = frame_count;
    const float* src = pcm;
    while (remaining > 0) {
        size_t space = audio::OPUS_FRAME_SIZE - secondary_pos_;
        size_t to_copy = std::min(static_cast<size_t>(remaining), space);
        for (size_t i = 0; i < to_copy; ++i)
            secondary_buf_[secondary_pos_ + i] = std::clamp(src[i] * gain, -1.0f, 1.0f);
        secondary_pos_ += to_copy;
        src += to_copy;
        remaining -= static_cast<int>(to_copy);

        if (secondary_pos_ >= static_cast<size_t>(audio::OPUS_FRAME_SIZE)) {
            int encoded = secondary_encoder_.encode(secondary_buf_.data(),
                                                    audio::OPUS_FRAME_SIZE,
                                                    secondary_opus_buf_,
                                                    audio::MAX_OPUS_PACKET);
            if (encoded > 0)
                on_secondary_encoded_frame(secondary_opus_buf_, static_cast<size_t>(encoded));
            secondary_pos_ = 0;
        }
    }
}

void AudioEngine::set_secondary_send_volume(float volume) {
    secondary_send_volume_.store(std::clamp(volume, 0.0f, 2.0f),
                                 std::memory_order_relaxed);
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
    cap_config.notificationCallback = capture_notification;

    if (selected_capture_ >= 0 && selected_capture_ < static_cast<int>(capture_ids_.size()))
        cap_config.capture.pDeviceID = &capture_ids_[selected_capture_];

    if (ma_device_init(&context_, &cap_config, &capture_device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize capture device");
        return false;
    }
    capture_initialized_ = true;

    // --- Playback device (stereo: voice mono→upmix + stream stereo) ---
    ma_device_config play_config = ma_device_config_init(ma_device_type_playback);
    play_config.playback.format = ma_format_f32;
    play_config.playback.channels = 2;
    play_config.sampleRate = audio::SAMPLE_RATE;
    play_config.dataCallback = AudioEngine::playback_callback;
    play_config.pUserData = this;
    play_config.periodSizeInMilliseconds = 10;
    play_config.notificationCallback = playback_notification;

    if (selected_playback_ >= 0 && selected_playback_ < static_cast<int>(playback_ids_.size()))
        play_config.playback.pDeviceID = &playback_ids_[selected_playback_];

    if (ma_device_init(&context_, &play_config, &playback_device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize playback device");
        ma_device_uninit(&capture_device_);
        capture_initialized_ = false;
        return false;
    }
    playback_initialized_ = true;

    capture_pos_ = 0;
    encode_pos_ = 0;

    LOG_INFO("Capture:  {} ({} Hz, {}ch, native={} Hz {}ch)",
             capture_device_.capture.name,
             capture_device_.sampleRate,
             capture_device_.capture.channels,
             capture_device_.capture.internalSampleRate,
             capture_device_.capture.internalChannels);
    LOG_INFO("Playback: {} ({} Hz, {}ch, native={} Hz {}ch)",
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
    echo_canceller_.shutdown();
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
    echo_canceller_.request_reset();
    if (ma_device_start(&playback_device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to start playback device");
        return false;
    }
    if (ma_device_start(&capture_device_) != MA_SUCCESS) {
        LOG_ERROR("Failed to start capture device");
        ma_device_stop(&playback_device_);
        return false;
    }
    running_ = true;
    LOG_INFO("Audio started");
    return true;
}

void AudioEngine::stop() {
    if (!running_) return;
    ma_device_stop(&capture_device_);
    ma_device_stop(&playback_device_);
    running_ = false;
    LOG_INFO("Audio stopped");
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

    engine->echo_canceller_.push_playback(static_cast<const float*>(output), frame_count);
}

void AudioEngine::process_capture(const float* input, ma_uint32 frame_count) {
	ZoneScopedN("AudioEngine::process_capture");
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
            echo_canceller_.process_capture(capture_buf_.data(), audio::FRAME_SIZE);

            // Keep AEC timing and adaptation alive while muted, but never run
            // the transmit pipeline or retain microphone samples.
            if (muted_.load(std::memory_order_relaxed) || !on_encoded_frame) {
                transmitting_.store(false, std::memory_order_relaxed);
                capture_pos_ = 0;
                continue;
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

            // VAD check (threshold is in perceptual/dB-normalized space)
            if (vad_enabled_) {
                float threshold = audio::perceptual_to_rms(vad_threshold_.load());
                if (rms >= threshold) {
                    vad_hold_frames_ = VAD_HOLD_COUNT;
                } else if (vad_hold_frames_ > 0) {
                    vad_hold_frames_--;
                } else {
                    // Below threshold and hold expired — don't encode
                    transmitting_ = false;
                    capture_pos_ = 0;
                    continue;
                }
            }
            transmitting_ = true;

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
    // Output is stereo interleaved (2 floats per frame)
    const ma_uint32 total_samples = frame_count * 2;
    std::memset(output, 0, total_samples * sizeof(float));

    // Mix voice audio into mono temp buffer, then upmix to stereo
    if (!deafened_ && mixer_) {
        // VoiceMixer outputs mono
        float mono_buf[4096];
        ma_uint32 remaining = frame_count;
        ma_uint32 offset = 0;
        while (remaining > 0) {
            ma_uint32 chunk = remaining > 4096 ? 4096 : remaining;
            std::memset(mono_buf, 0, chunk * sizeof(float));
            mixer_->mix_output(mono_buf, static_cast<int>(chunk));

            // Voice normalization (per-chunk)
            if (normalize_enabled_) {
                float sum = 0.0f;
                for (ma_uint32 i = 0; i < chunk; i++)
                    sum += mono_buf[i] * mono_buf[i];
                float rms = std::sqrt(sum / chunk);

                if (rms > 0.001f) {
                    float slider = normalize_target_.load();
                    float target = slider * slider * slider; // Power curve for perceptual linearity
                    float desired_gain = target / rms;
                    desired_gain = std::clamp(desired_gain, 0.1f, 10.0f);
                    float alpha = 0.05f;
                    current_gain_ += alpha * (desired_gain - current_gain_);

                    for (ma_uint32 i = 0; i < chunk; i++) {
                        mono_buf[i] *= current_gain_;
                        if (mono_buf[i] > 1.0f) mono_buf[i] = 1.0f;
                        else if (mono_buf[i] < -1.0f) mono_buf[i] = -1.0f;
                    }
                }
            }

            // Upmix mono to stereo (duplicate to both channels)
            for (ma_uint32 i = 0; i < chunk; i++) {
                output[(offset + i) * 2 + 0] = mono_buf[i];
                output[(offset + i) * 2 + 1] = mono_buf[i];
            }
            offset += chunk;
            remaining -= chunk;
        }
    }

    // Mix the secondary/auxiliary voice stream (karaoke/join sounds/plugin audio).
    // It is mono like the primary mix, but gets NO voice normalization or makeup
    // gain — the aux VoiceMixer already applies its own (no-makeup) master volume.
    if (!deafened_ && aux_mixer_) {
        float mono_buf[4096];
        ma_uint32 remaining = frame_count;
        ma_uint32 offset = 0;
        while (remaining > 0) {
            ma_uint32 chunk = remaining > 4096 ? 4096 : remaining;
            std::memset(mono_buf, 0, chunk * sizeof(float));
            aux_mixer_->mix_output(mono_buf, static_cast<int>(chunk));
            for (ma_uint32 i = 0; i < chunk; i++) {
                output[(offset + i) * 2 + 0] += mono_buf[i];
                output[(offset + i) * 2 + 1] += mono_buf[i];
            }
            offset += chunk;
            remaining -= chunk;
        }
    }

    // Mix stream audio (already stereo, adds to existing output)
    if (stream_player_)
        stream_player_->mix_output(output, static_cast<int>(frame_count));

    // Final hard limiter across the summed output. Each source self-clamps its own
    // contribution to [-1,1], but primary voice + secondary (VOICE2) + screen-share
    // audio are summed on top of each other and can exceed full scale together —
    // clamp once at the end so loud overlap doesn't wrap/clip at the device.
    for (ma_uint32 i = 0; i < total_samples; i++) {
        if (output[i] > 1.0f) output[i] = 1.0f;
        else if (output[i] < -1.0f) output[i] = -1.0f;
    }
}

} // namespace parties::client
