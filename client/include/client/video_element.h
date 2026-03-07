#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>
#include <vector>

namespace parties::client {

// Custom RmlUi element that renders video frames as GPU textures.
// Supports two rendering paths:
// 1. RGBA: CPU-converted frames uploaded as a single RGBA texture
// 2. YUV:  I420 planes uploaded as 3 R8 textures, converted to RGB by pixel shader (GPU)
class VideoElement : public Rml::Element {
public:
    explicit VideoElement(const Rml::String& tag);
    ~VideoElement() override;

    // Upload I420 YUV planes directly — GPU converts to RGB in pixel shader.
    // This is the fastest path: no CPU conversion, 2.67× less upload bandwidth.
    void UpdateYUVFrame(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

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

    // Compiled quad geometry
    Rml::CompiledGeometryHandle video_geom_ = 0;
    float geom_w_ = 0;
    float geom_h_ = 0;
};

class VideoElementInstancer : public Rml::ElementInstancer {
public:
    VideoElementInstancer() = default;
    Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag,
                                     const Rml::XMLAttributes& attributes) override;
    void ReleaseElement(Rml::Element* element) override;
};

} // namespace parties::client
