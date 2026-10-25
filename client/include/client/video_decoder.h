#pragma once

#include <encdec/decoder.h>

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

struct ID3D12Device;

namespace parties::client {

using encdec::DecodedFrame;

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    bool init(VideoCodecId codec, uint32_t width, uint32_t height,
              ::ID3D12Device* render_device = nullptr,
              std::stop_token stop_token = {});
    void shutdown();

    bool decode(const uint8_t* data, size_t len, int64_t timestamp);

    void flush();

    VideoCodecId codec() const { return codec_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    bool context_lost() const;

    void disable_hardware() { hardware_disabled_ = true; }

    const char* backend_name() const;

    std::function<void(const DecodedFrame& frame)> on_decoded;

private:
    std::unique_ptr<encdec::Decoder> decoder_;
    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool hardware_disabled_ = false;
    std::chrono::steady_clock::time_point last_slow_decode_log_{};
    uint32_t suppressed_slow_decodes_ = 0;
};

}
