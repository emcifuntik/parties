#pragma once

#include <parties/codec.h>
#include <parties/audio_common.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <variant>
#include <vector>

namespace parties::client {

struct CaptureAllExceptOwnProcess {};
struct CaptureProcessTree { uint32_t pid; };
using StreamAudioSource = std::variant<CaptureAllExceptOwnProcess, CaptureProcessTree>;

// Uses ActivateAudioInterfaceAsync for process-specific capture (Win10 2004+).
class StreamAudioCapture {
public:
    enum class OutputMode {
        EncodedStereo,
        MonoPcm,
    };

    StreamAudioCapture();
    ~StreamAudioCapture();

    bool init(StreamAudioSource source, OutputMode mode = OutputMode::EncodedStereo);
    void shutdown();

    bool start();
    void stop();

    std::function<void(const uint8_t* opus_data, size_t opus_len)> on_encoded_frame;
    std::function<void(const float* mono_pcm, int frame_count)> on_pcm_frame;

private:
    void capture_thread_func();

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kFrameSize = 960;

    struct WasapiState;
    WasapiState* wasapi_ = nullptr;

    std::thread capture_thread_;
    std::atomic<bool> running_{false};

    OpusCodec encoder_;
    bool encoder_initialized_ = false;
    OutputMode output_mode_ = OutputMode::EncodedStereo;

    std::vector<float> capture_buf_;
    std::vector<float> mono_buf_;
    size_t capture_pos_ = 0;

    uint8_t opus_buf_[audio::MAX_OPUS_PACKET];
};

}
