#include <client/stream_audio_player.h>
#include <parties/profiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace parties::client {

static void stream_playback_notification(const ma_device_notification* pNotification) {
    if (pNotification->type == ma_device_notification_type_started)
        TracySetThreadName("StreamAudioPlayback");
}

StreamAudioPlayer::StreamAudioPlayer() = default;

StreamAudioPlayer::~StreamAudioPlayer() {
    shutdown();
}

bool StreamAudioPlayer::init() {
    if (!decoder_.init_decoder(kSampleRate, kChannels)) {
        std::fprintf(stderr, "[StreamAudio] Failed to init Opus decoder\n");
        return false;
    }
    decoder_initialized_ = true;

    pcm_buf_.resize(kFrameSize * kChannels, 0.0f);
    pcm_pos_ = kFrameSize * kChannels;  // Force decode on first read

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kSampleRate;
    config.dataCallback = StreamAudioPlayer::data_callback;
    config.pUserData = this;
    config.periodSizeInMilliseconds = 10;
    config.notificationCallback = stream_playback_notification;

    if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[StreamAudio] Failed to init playback device\n");
        return false;
    }
    device_initialized_ = true;

    std::printf("[StreamAudio] Device: %s (%d Hz, %d ch)\n",
                device_.playback.name,
                device_.playback.internalSampleRate,
                device_.playback.internalChannels);

    if (ma_device_start(&device_) != MA_SUCCESS) {
        std::fprintf(stderr, "[StreamAudio] Failed to start playback device\n");
        ma_device_uninit(&device_);
        device_initialized_ = false;
        return false;
    }

    return true;
}

void StreamAudioPlayer::shutdown() {
    if (device_initialized_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        device_initialized_ = false;
    }
    decoder_initialized_ = false;
}

void StreamAudioPlayer::push_packet(const uint8_t* opus_data, size_t opus_len) {
	ZoneScopedN("StreamAudioPlayer::push_packet");
    std::lock_guard<std::mutex> lock(mutex_);

    if (packet_queue_.size() >= kMaxJitterPackets)
        packet_queue_.pop_front();

    packet_queue_.emplace_back(opus_data, opus_data + opus_len);
    consecutive_empty_ = 0;
}

void StreamAudioPlayer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    packet_queue_.clear();
    primed_ = false;
    consecutive_empty_ = 0;
    pcm_pos_ = kFrameSize * kChannels;
}

void StreamAudioPlayer::set_volume(float vol) {
    volume_.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed);
}

void StreamAudioPlayer::data_callback(ma_device* device, void* output,
                                       const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<StreamAudioPlayer*>(device->pUserData);
    self->process_playback(static_cast<float*>(output), frame_count);
}

void StreamAudioPlayer::process_playback(float* output, ma_uint32 frame_count) {
	ZoneScopedN("StreamAudioPlayer::process_playback");
    const int total_samples = frame_count * kChannels;
    std::memset(output, 0, total_samples * sizeof(float));

    if (!decoder_initialized_) return;

    float vol = volume_.load(std::memory_order_relaxed);
    int written = 0;

    while (written < static_cast<int>(frame_count)) {
        // If current buffer exhausted, decode next frame
        if (pcm_pos_ >= static_cast<size_t>(kFrameSize * kChannels)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!decode_frame(pcm_buf_.data(), kFrameSize)) {
                break;  // No audio available
            }
            pcm_pos_ = 0;
        }

        int available = (kFrameSize * kChannels - static_cast<int>(pcm_pos_)) / kChannels;
        int needed = static_cast<int>(frame_count) - written;
        int chunk = std::min(available, needed);

        for (int i = 0; i < chunk * kChannels; i++) {
            float s = pcm_buf_[pcm_pos_ + i] * vol;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            output[written * kChannels + i] = s;
        }

        pcm_pos_ += chunk * kChannels;
        written += chunk;
    }
}

bool StreamAudioPlayer::decode_frame(float* pcm_out, int frame_size) {
    // Must be called with mutex_ held

    if (!primed_ && packet_queue_.size() < kPreBuffer)
        return false;

    primed_ = true;

    if (!packet_queue_.empty()) {
        auto& pkt = packet_queue_.front();
        int decoded = decoder_.decode(pkt.data(), static_cast<int>(pkt.size()),
                                       pcm_out, frame_size);
        packet_queue_.pop_front();
        consecutive_empty_ = 0;
        return decoded > 0;
    }

    // No packet — try PLC
    consecutive_empty_++;
    if (consecutive_empty_ <= kPlcMaxFrames) {
        int decoded = decoder_.decode(nullptr, 0, pcm_out, frame_size);
        return decoded > 0;
    }

    // Too many PLC frames — reset
    primed_ = false;
    return false;
}

} // namespace parties::client
