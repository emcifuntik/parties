// NVDEC hardware decoder for AV1, using CUDA + CUVID parser.
// Falls back gracefully — caller should try dav1d if init() returns false.
#pragma once

#include "nvidia_loader.h"
#include <client/video_decoder.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace parties::client::nvidia {

class NvdecDecoder {
public:
    NvdecDecoder();
    ~NvdecDecoder();

    // Initialize NVDEC for AV1. Returns false if hardware unavailable.
    bool init(uint32_t width, uint32_t height);
    void shutdown();

    // Feed encoded AV1 data. Calls on_decoded when frames are ready.
    bool decode(const uint8_t* data, size_t len, int64_t timestamp);

    // Flush buffered frames
    void flush();

    // Callback with decoded NV12 frame
    std::function<void(const parties::client::DecodedFrame& frame)> on_decoded;

private:
    // CUVID parser callbacks (static, forward to instance)
    static int CUDAAPI handle_sequence(void* user, CUVIDEOFORMAT* fmt);
    static int CUDAAPI handle_decode(void* user, CUVIDPICPARAMS* pic);
    static int CUDAAPI handle_display(void* user, CUVIDPARSERDISPINFO* info);

    // Instance handlers
    int on_sequence(CUVIDEOFORMAT* fmt);
    int on_decode(CUVIDPICPARAMS* pic);
    int on_display(CUVIDPARSERDISPINFO* info);

    CudaApi cuda_{};
    CuvidApi cuvid_{};

    CUcontext cu_ctx_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bit_depth_ = 8;
    uint32_t num_decode_surfaces_ = 0;

    // Pinned host memory for NV12 download
    uint8_t* pinned_nv12_ = nullptr;
    size_t pinned_nv12_size_ = 0;

    bool initialized_ = false;
};

} // namespace parties::client::nvidia
