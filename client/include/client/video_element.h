#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>
#include <memory>
#include <vector>

class ExtendedRenderInterface;
namespace Rml { class Context; }

namespace parties::client {

class VideoElement : public Rml::Element {
public:
    explicit VideoElement(const Rml::String& tag);
    ~VideoElement() override;

    void UpdateYUVFrame(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    void UpdateYUVFrame(
        std::vector<uint8_t>& y_data, uint32_t y_stride,
        std::vector<uint8_t>& u_data, std::vector<uint8_t>& v_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    void UpdateNV12Frame(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    void UpdateNV12Frame(
        std::vector<uint8_t>& y_data, uint32_t y_stride,
        std::vector<uint8_t>& uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height);

    void UpdateNativeNV12Frame(
        void* d3d12_resource, void* d3d12_chroma_resource,
        std::shared_ptr<void> owner,
        void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
        uint32_t width, uint32_t height,
        uint32_t texture_width = 0, uint32_t texture_height = 0,
        uint32_t crop_x = 0, uint32_t crop_y = 0);

    void UpdateFrame(std::vector<uint8_t>&& rgba_data, uint32_t width, uint32_t height);

    void UpdateFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height);

    void SetVideoDimensions(uint32_t width, uint32_t height);
    void Clear();

    uint32_t frame_width() const { return frame_width_; }
    uint32_t frame_height() const { return frame_height_; }

    static void RegisterContextRenderInterface(Rml::Context* context, ExtendedRenderInterface* renderer);
    static void UnregisterContextRenderInterface(Rml::Context* context);

protected:
    bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;
    void OnRender() override;
    void OnResize() override;

private:
    ExtendedRenderInterface* ResolveRenderInterface();
    void ReleaseResources();
    void RebuildGeometry();
    void SetTextureCrop(uint32_t texture_width, uint32_t texture_height,
                        uint32_t crop_x, uint32_t crop_y,
                        uint32_t visible_width, uint32_t visible_height);

    ExtendedRenderInterface* render_interface_ = nullptr;
    uint32_t frame_width_ = 0;
    uint32_t frame_height_ = 0;
    bool has_frame_ = false;

    std::vector<uint8_t> frame_data_;
    Rml::TextureHandle video_texture_ = 0;
    uint32_t texture_w_ = 0;
    uint32_t texture_h_ = 0;
    bool texture_dirty_ = false;

    bool yuv_mode_ = false;
    uintptr_t yuv_texture_ = 0;
    uint32_t yuv_tex_w_ = 0;
    uint32_t yuv_tex_h_ = 0;
    bool yuv_dirty_ = false;

    std::vector<uint8_t> yuv_y_, yuv_u_, yuv_v_;
    uint32_t yuv_y_stride_ = 0, yuv_uv_stride_ = 0;

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

    Rml::CompiledGeometryHandle video_geom_ = 0;
    float geom_w_ = 0;
    float geom_h_ = 0;
    float texture_u0_ = 0.0f;
    float texture_v0_ = 0.0f;
    float texture_u1_ = 1.0f;
    float texture_v1_ = 1.0f;
    bool geometry_uv_dirty_ = false;
};

}
