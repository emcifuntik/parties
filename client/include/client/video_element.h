#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace parties::client {

// Custom RmlUi element that renders video frames as GPU textures.
// Supports two rendering paths:
// 1. RGBA: frames uploaded or shared as a single RGBA texture.
// 2. NV12: luma/chroma planes sampled directly by the final DX12 draw.
// I420 retains the existing compute conversion compatibility path.
class VideoElement : public Rml::Element {
public:
    explicit VideoElement(const Rml::String& tag);
    ~VideoElement() override;

    // Upload I420 YUV planes directly — GPU converts to RGB in pixel shader.
    void UpdateYUVFrame(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    // Swap-based I420 upload. As with the NV12 overload, the caller receives
    // our previous buffers for reuse instead of copying three planes again.
    void UpdateYUVFrame(
        std::vector<uint8_t>& y_data, uint32_t y_stride,
        std::vector<uint8_t>& u_data, std::vector<uint8_t>& v_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    // Upload NV12 frame (Y + interleaved UV) — native hardware decoder format.
    // No CPU deinterleave needed; GPU shader handles NV12 → RGB.
    void UpdateNV12Frame(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    // Swap-based NV12 upload — exchanges buffers with caller, zero memcpy.
    // Caller's old buffers cycle back through the swap chain for reuse.
    void UpdateNV12Frame(
        std::vector<uint8_t>& y_data, uint32_t y_stride,
        std::vector<uint8_t>& uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    // Bind a decoder-owned ID3D12 NV12 or RGBA resource without host readback
    // or upload. `owner` reserves it until the renderer fence has retired.
    void UpdateNativeNV12Frame(
        void* d3d12_resource, void* d3d12_chroma_resource,
        std::shared_ptr<void> owner,
        void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
        uint32_t width, uint32_t height,
        uint32_t texture_width = 0, uint32_t texture_height = 0,
        uint32_t crop_x = 0, uint32_t crop_y = 0);

    // Upload a new RGBA video frame (move semantics — zero-copy from caller).
    void UpdateFrame(std::vector<uint8_t>&& rgba_data, uint32_t width, uint32_t height);

    // Upload a new RGBA video frame (copy from pointer).
    void UpdateFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height);

    // Set layout dimensions without pixel data (for placeholder sizing).
    void SetVideoDimensions(uint32_t width, uint32_t height);
    void Clear();

    uint32_t frame_width() const { return frame_width_; }
    uint32_t frame_height() const { return frame_height_; }

protected:
    bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;
    void OnRender() override;
    void OnResize() override;

private:
    void ReleaseResources();
    void RebuildGeometry();
    void SetTextureCrop(uint32_t texture_width, uint32_t texture_height,
                        uint32_t crop_x, uint32_t crop_y,
                        uint32_t visible_width, uint32_t visible_height);

    uint32_t frame_width_ = 0;
    uint32_t frame_height_ = 0;
    bool has_frame_ = false;

    // RGBA path
    std::vector<uint8_t> frame_data_;
    Rml::TextureHandle video_texture_ = 0;
    uint32_t texture_w_ = 0;
    uint32_t texture_h_ = 0;
    bool texture_dirty_ = false;

    // YUV path (GPU conversion via pixel shader)
    bool yuv_mode_ = false;
    uintptr_t yuv_texture_ = 0;
    uint32_t yuv_tex_w_ = 0;
    uint32_t yuv_tex_h_ = 0;
    bool yuv_dirty_ = false;

    // I420 plane data (held until OnRender uploads to GPU)
    std::vector<uint8_t> yuv_y_, yuv_u_, yuv_v_;
    uint32_t yuv_y_stride_ = 0, yuv_uv_stride_ = 0;

    // NV12 mode (Y + interleaved UV, native hw decoder format)
    bool nv12_mode_ = false;
    uintptr_t nv12_texture_ = 0;
    uint32_t nv12_tex_w_ = 0, nv12_tex_h_ = 0;
    bool nv12_dirty_ = false;
    std::vector<uint8_t> nv12_y_, nv12_uv_;
    uint32_t nv12_y_stride_ = 0, nv12_uv_stride_ = 0;
    void* nv12_native_resource_ = nullptr;
    void* nv12_native_chroma_resource_ = nullptr;
    void* nv12_native_ready_fence_ = nullptr;
    uint64_t nv12_native_ready_value_ = 0;
    uint32_t nv12_native_resource_state_ = 0;
    bool nv12_native_rgba_ = false;
    std::shared_ptr<void> nv12_native_owner_;
    bool nv12_native_mode_ = false;
    bool nv12_texture_native_ = false;

    // Compiled quad geometry
    Rml::CompiledGeometryHandle video_geom_ = 0;
    float geom_w_ = 0;
    float geom_h_ = 0;
    float texture_u0_ = 0.0f;
    float texture_v0_ = 0.0f;
    float texture_u1_ = 1.0f;
    float texture_v1_ = 1.0f;
    bool geometry_uv_dirty_ = false;
};

} // namespace parties::client
