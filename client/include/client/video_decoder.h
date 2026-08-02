#pragma once

#include <encdec/decoder.h>

#include <cstdint>
#include <functional>
#include <memory>

struct ID3D12Device;

namespace parties::client {

// Re-export DecodedFrame from encdec for client code compatibility
using encdec::DecodedFrame;

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    // Initialize for the given codec.
    // Tries hardware decoders first (NVDEC, AMF), then software (dav1d, MFT).
    bool init(VideoCodecId codec, uint32_t width, uint32_t height,
              ::ID3D12Device* render_device = nullptr);
    void shutdown();

    // Feed encoded data. Calls on_decoded when a frame is ready.
    bool decode(const uint8_t* data, size_t len, int64_t timestamp);

    // Flush any buffered frames
    void flush();

    VideoCodecId codec() const { return codec_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // True if the hardware decoder's GPU context was invalidated
    bool context_lost() const;

    // After a context_lost, call this before reinit to force software-only decoding.
    void disable_hardware() { hardware_disabled_ = true; }

    // Which backend is active?
    const char* backend_name() const;

    // Callback with decoded frame
    std::function<void(const DecodedFrame& frame)> on_decoded;

private:
    std::unique_ptr<encdec::Decoder> decoder_;
    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool hardware_disabled_ = false;
};

} // namespace parties::client
