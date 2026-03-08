#pragma once

#include <parties/codec.h>
#include <parties/audio_common.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace parties::client {

// Decodes and buffers screen share audio (Opus stereo).
// Does NOT own a playback device — AudioEngine pulls samples via mix_output().
class StreamAudioPlayer {
public:
    StreamAudioPlayer();
    ~StreamAudioPlayer();

    bool init();
    void shutdown();

    // Feed an incoming Opus packet from the stream sharer
    void push_packet(const uint8_t* opus_data, size_t opus_len);

    // Mix decoded stereo audio into output buffer (called from AudioEngine's playback callback).
    // output is stereo interleaved, frame_count is number of stereo frames.
    // Adds to existing content (does not zero first).
    void mix_output(float* output, int frame_count);

    // Clear all buffered audio (e.g. when switching sharers)
    void clear();

    // Volume control (0.0 - 2.0, default 1.0)
    void set_volume(float vol);
    float volume() const { return volume_.load(std::memory_order_relaxed); }

private:
    bool decode_frame(float* pcm_out, int frame_size);

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;  // stereo
    static constexpr int kFrameSize = 960;  // 20ms at 48kHz (matches Opus)
    static constexpr int kMaxJitterPackets = 10;
    static constexpr int kPreBuffer = 3;
    static constexpr int kPlcMaxFrames = 10;

    OpusCodec decoder_;
    bool decoder_initialized_ = false;

    std::mutex mutex_;
    std::deque<std::vector<uint8_t>> packet_queue_;
    bool primed_ = false;
    int consecutive_empty_ = 0;

    // Decoded PCM buffer for partial reads (stereo interleaved)
    std::vector<float> pcm_buf_;
    size_t pcm_pos_ = 0;

    std::atomic<float> volume_{1.0f};
};

} // namespace parties::client
