#pragma once

#include <parties/video_common.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace parties::client::nvidia { class NvdecDecoder; }

namespace parties::client {

struct DecodedFrame {
    const uint8_t* y_plane;
    const uint8_t* u_plane;   // I420: U plane; NV12: interleaved UV plane
    const uint8_t* v_plane;   // I420: V plane; NV12: unused (nullptr)
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t width;
    uint32_t height;
    int64_t timestamp;
    bool nv12 = false;        // true = NV12 (Y + interleaved UV), false = I420 (Y + U + V)
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Initialize for the given codec. Width/height are hints for MFT;
    // dav1d determines dimensions from the bitstream.
    bool init(VideoCodecId codec, uint32_t width, uint32_t height);
    void shutdown();

    // Feed encoded data. Calls on_decoded when a frame is ready.
    bool decode(const uint8_t* data, size_t len, int64_t timestamp);

    // Flush any buffered frames
    void flush();

    VideoCodecId codec() const { return codec_; }

    // True if the hardware decoder's GPU context was invalidated
    // (e.g., game launch causing device reset). Caller should reinitialize.
    bool context_lost() const;

    // After a context_lost, call this before reinit to force software-only decoding.
    void disable_hardware() { hardware_disabled_ = true; }

    // Callback with decoded I420 frame
    std::function<void(const DecodedFrame& frame)> on_decoded;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<nvidia::NvdecDecoder> nvdec_;
    VideoCodecId codec_ = VideoCodecId::AV1;
    bool initialized_ = false;
    bool hardware_disabled_ = false;
};

} // namespace parties::client
