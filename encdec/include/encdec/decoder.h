#pragma once

#include <encdec/codec.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace parties::encdec {

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

    // Optional zero-host-copy output. On Windows this is a vendor-produced
    // ID3D12Resource consumable by the renderer. native_owner reserves the
    // resource and vendor state until the render backend's GPU fence completes.
    void* native_d3d12_resource = nullptr;
    // Optional standalone DXGI_FORMAT_R8G8_UNORM chroma plane. The primary
    // resource may be DXGI_FORMAT_R8_UNORM luma when a vendor's native NV12
    // layout cannot be exposed as a portable D3D12 multi-plane resource.
    void* native_d3d12_chroma_resource = nullptr;
    // Optional producer timeline fence. D3D12 waits for native_fence_value
    // before sampling native_d3d12_resource; native_owner keeps both objects
    // and any vendor interop state alive through GPU completion.
    void* native_d3d12_fence = nullptr;
    uint64_t native_fence_value = 0;
    // Resource state reported by the producer. Consumers restore this state
    // after conversion before releasing native_owner back to the decoder.
    uint32_t native_d3d12_state = 0;
    // Physical dimensions and visible origin of a native decoder surface.
    // Opaque NVDEC output is coded-size (for example 1920x1088 for 1080p),
    // so the final draw crops it through texture coordinates without a copy.
    uint32_t native_texture_width = 0;
    uint32_t native_texture_height = 0;
    uint32_t native_crop_x = 0;
    uint32_t native_crop_y = 0;
    // True when the native resource is packed RGBA8. False means packed NV12;
    // the DX12 renderer binds its luma/chroma planes directly in the final draw.
    bool native_rgba = false;
    std::shared_ptr<void> native_owner;
};

struct DecoderInfo {
    Backend backend;
    VideoCodecId codec;
};

class Decoder {
public:
    virtual ~Decoder() = default;

    // Feed encoded data. Fires on_decoded when a frame is ready.
    virtual bool decode(const uint8_t* data, size_t len, int64_t timestamp) = 0;

    // Flush any buffered frames
    virtual void flush() = 0;

    // True if the GPU context was invalidated (device reset, game launch, TDR).
    // Caller should destroy this decoder and create a new one.
    virtual bool context_lost() const { return false; }

    // Metadata about the active decoder
    virtual DecoderInfo info() const = 0;

    // Callback with decoded frame
    std::function<void(const DecodedFrame& frame)> on_decoded;

    // Convenience
    VideoCodecId codec() const { return info().codec; }
};

} // namespace parties::encdec
