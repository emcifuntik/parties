#include <client/stream_audio_player.h>
#include <parties/profiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace parties::client {

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

    return true;
}

void StreamAudioPlayer::shutdown() {
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

void StreamAudioPlayer::mix_output(float* output, int frame_count) {
	ZoneScopedN("StreamAudioPlayer::mix_output");

    if (!decoder_initialized_) return;

    float vol = volume_.load(std::memory_order_relaxed);
    int written = 0;

    while (written < frame_count) {
        // If current buffer exhausted, decode next frame
        if (pcm_pos_ >= static_cast<size_t>(kFrameSize * kChannels)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!decode_frame(pcm_buf_.data(), kFrameSize)) {
                break;  // No audio available
            }
            pcm_pos_ = 0;
        }

        int available = (kFrameSize * kChannels - static_cast<int>(pcm_pos_)) / kChannels;
        int needed = frame_count - written;
        int chunk = std::min(available, needed);

        for (int i = 0; i < chunk * kChannels; i++) {
            float s = pcm_buf_[pcm_pos_ + i] * vol;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            output[written * kChannels + i] += s;
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
