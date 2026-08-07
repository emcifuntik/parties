#include <client/video_element.h>

#include "RmlUi_RenderInterface_Extended.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <parties/profiler.h>

namespace parties::client {

// ── VideoElement ────────────────────────────────────────────────────

VideoElement::VideoElement(const Rml::String& tag)
    : Rml::Element(tag) {}

VideoElement::~VideoElement() {
    ReleaseResources();
}

void VideoElement::SetTextureCrop(uint32_t texture_width, uint32_t texture_height,
                                  uint32_t crop_x, uint32_t crop_y,
                                  uint32_t visible_width, uint32_t visible_height) {
    texture_width = texture_width ? texture_width : visible_width;
    texture_height = texture_height ? texture_height : visible_height;
    if (!texture_width || !texture_height) return;

    crop_x = (std::min)(crop_x, texture_width);
    crop_y = (std::min)(crop_y, texture_height);
    const uint32_t right = (std::min)(crop_x + visible_width, texture_width);
    const uint32_t bottom = (std::min)(crop_y + visible_height, texture_height);
    const float u0 = static_cast<float>(crop_x) / texture_width;
    const float v0 = static_cast<float>(crop_y) / texture_height;
    const float u1 = static_cast<float>(right) / texture_width;
    const float v1 = static_cast<float>(bottom) / texture_height;
    if (u0 != texture_u0_ || v0 != texture_v0_ ||
        u1 != texture_u1_ || v1 != texture_v1_) {
        texture_u0_ = u0;
        texture_v0_ = v0;
        texture_u1_ = u1;
        texture_v1_ = v1;
        geometry_uv_dirty_ = true;
    }
}

void VideoElement::UpdateYUVFrame(
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* u_data, const uint8_t* v_data, uint32_t uv_stride,
    uint32_t width, uint32_t height) {
	ZoneScopedN("VideoElement::UpdateYUVFrame");

    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_ = width;
    frame_height_ = height;
    has_frame_ = true;
    yuv_mode_ = true;
    nv12_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);

    // Store plane data — uploaded to GPU in OnRender (must happen on render thread)
    uint32_t half_h = height / 2;

    yuv_y_.resize(static_cast<size_t>(y_stride) * height);
    std::memcpy(yuv_y_.data(), y_data, yuv_y_.size());

    yuv_u_.resize(static_cast<size_t>(uv_stride) * half_h);
    std::memcpy(yuv_u_.data(), u_data, yuv_u_.size());

    yuv_v_.resize(static_cast<size_t>(uv_stride) * half_h);
    std::memcpy(yuv_v_.data(), v_data, yuv_v_.size());

    yuv_y_stride_ = y_stride;
    yuv_uv_stride_ = uv_stride;
    yuv_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateYUVFrame(
    std::vector<uint8_t>& y_data, uint32_t y_stride,
    std::vector<uint8_t>& u_data, std::vector<uint8_t>& v_data, uint32_t uv_stride,
    uint32_t width, uint32_t height) {
    ZoneScopedN("VideoElement::UpdateYUVFrame(swap)");

    const bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_ = width;
    frame_height_ = height;
    has_frame_ = true;
    yuv_mode_ = true;
    nv12_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);

    yuv_y_.swap(y_data);
    yuv_u_.swap(u_data);
    yuv_v_.swap(v_data);
    yuv_y_stride_ = y_stride;
    yuv_uv_stride_ = uv_stride;
    yuv_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateNV12Frame(
    const uint8_t* y_data, uint32_t y_stride,
    const uint8_t* uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height) {
	ZoneScopedN("VideoElement::UpdateNV12Frame");

    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_ = width;
    frame_height_ = height;
    has_frame_ = true;
    nv12_mode_ = true;
    yuv_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);

    uint32_t half_h = height / 2;

    nv12_y_.resize(static_cast<size_t>(y_stride) * height);
    std::memcpy(nv12_y_.data(), y_data, nv12_y_.size());

    nv12_uv_.resize(static_cast<size_t>(uv_stride) * half_h);
    std::memcpy(nv12_uv_.data(), uv_data, nv12_uv_.size());

    nv12_y_stride_ = y_stride;
    nv12_uv_stride_ = uv_stride;
    nv12_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateNV12Frame(
    std::vector<uint8_t>& y_data, uint32_t y_stride,
    std::vector<uint8_t>& uv_data, uint32_t uv_stride,
    uint32_t width, uint32_t height) {
	ZoneScopedN("VideoElement::UpdateNV12Frame(swap)");

    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_ = width;
    frame_height_ = height;
    has_frame_ = true;
    nv12_mode_ = true;
    yuv_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);

    // Swap instead of move — caller gets our old buffer back,
    // which cycles through the swap chain and avoids malloc every frame.
    nv12_y_.swap(y_data);
    nv12_uv_.swap(uv_data);
    nv12_y_stride_ = y_stride;
    nv12_uv_stride_ = uv_stride;
    nv12_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateNativeNV12Frame(
    void* d3d12_resource, void* d3d12_chroma_resource,
    std::shared_ptr<void> owner,
    void* ready_fence, uint64_t ready_value, uint32_t resource_state, bool rgba,
    uint32_t width, uint32_t height,
    uint32_t texture_width, uint32_t texture_height,
    uint32_t crop_x, uint32_t crop_y) {
    ZoneScopedN("VideoElement::UpdateNativeNV12Frame");
    if (!d3d12_resource || !owner) return;

    const bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_ = width;
    frame_height_ = height;
    has_frame_ = true;
    nv12_mode_ = true;
    yuv_mode_ = false;
    nv12_native_mode_ = true;
    nv12_native_resource_ = d3d12_resource;
    nv12_native_chroma_resource_ = d3d12_chroma_resource;
    nv12_native_ready_fence_ = ready_fence;
    nv12_native_ready_value_ = ready_value;
    nv12_native_resource_state_ = resource_state;
    nv12_native_rgba_ = rgba;
    nv12_native_owner_ = std::move(owner);
    SetTextureCrop(texture_width, texture_height, crop_x, crop_y, width, height);
    nv12_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateFrame(std::vector<uint8_t>&& rgba_data, uint32_t width, uint32_t height) {
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;
    yuv_mode_ = false;
    nv12_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);
    frame_data_ = std::move(rgba_data);
    texture_dirty_ = true;
    if (size_changed)
        DirtyLayout();
}

void VideoElement::UpdateFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height) {
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;
    yuv_mode_ = false;
    nv12_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    SetTextureCrop(width, height, 0, 0, width, height);

    size_t byte_count = static_cast<size_t>(width) * height * 4;
    frame_data_.resize(byte_count);
    std::memcpy(frame_data_.data(), rgba_data, byte_count);
    texture_dirty_ = true;

    if (size_changed)
        DirtyLayout();
}

void VideoElement::SetVideoDimensions(uint32_t width, uint32_t height) {
	ZoneScopedN("VideoElement::SetVideoDimensions");
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;
    SetTextureCrop(width, height, 0, 0, width, height);

    if (size_changed)
        DirtyLayout();
}

void VideoElement::Clear() {
    has_frame_ = false;
    frame_width_  = 0;
    frame_height_ = 0;
    frame_data_.clear();
    texture_dirty_ = false;
    yuv_mode_ = false;
    yuv_dirty_ = false;
    yuv_y_.clear();
    yuv_u_.clear();
    yuv_v_.clear();
    nv12_mode_ = false;
    nv12_native_mode_ = false;
    nv12_native_resource_ = nullptr;
    nv12_native_chroma_resource_ = nullptr;
    nv12_native_ready_fence_ = nullptr;
    nv12_native_ready_value_ = 0;
    nv12_native_resource_state_ = 0;
    nv12_native_rgba_ = false;
    nv12_native_owner_.reset();
    nv12_dirty_ = false;
    nv12_y_.clear();
    nv12_uv_.clear();
    SetTextureCrop(1, 1, 0, 0, 1, 1);
    ReleaseResources();
    DirtyLayout();
}

void VideoElement::ReleaseResources() {
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    if (video_texture_) {
        ri->ReleaseTexture(video_texture_);
        video_texture_ = 0;
    }
    texture_w_ = texture_h_ = 0;

    if (yuv_texture_) {
        static_cast<ExtendedRenderInterface*>(ri)->ReleaseYUVTexture(yuv_texture_);
        yuv_texture_ = 0;
    }
    yuv_tex_w_ = yuv_tex_h_ = 0;

    if (nv12_texture_) {
        static_cast<ExtendedRenderInterface*>(ri)->ReleaseNV12Texture(nv12_texture_);
        nv12_texture_ = 0;
    }
    nv12_tex_w_ = nv12_tex_h_ = 0;
    nv12_texture_native_ = false;

    if (video_geom_) {
        ri->ReleaseGeometry(video_geom_);
        video_geom_ = 0;
    }
    geom_w_ = geom_h_ = 0;
}

bool VideoElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) {
    if (frame_width_ > 0 && frame_height_ > 0) {
        dimensions.x = static_cast<float>(frame_width_);
        dimensions.y = static_cast<float>(frame_height_);
        ratio = dimensions.x / dimensions.y;
        return true;
    }
    return false;
}

void VideoElement::RebuildGeometry() {
	ZoneScopedN("VideoElement::RebuildGeometry");
    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;

    if (video_geom_) {
        ri->ReleaseGeometry(video_geom_);
        video_geom_ = 0;
    }

    // Compute aspect-ratio-preserving rect within element content box
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    float elem_w = size.x, elem_h = size.y;
    if (elem_w <= 0 || elem_h <= 0) return;

    float video_aspect = static_cast<float>(frame_width_) / static_cast<float>(frame_height_);
    float elem_aspect  = elem_w / elem_h;

    float rw, rh;
    if (video_aspect > elem_aspect) {
        rw = elem_w;
        rh = elem_w / video_aspect;
    } else {
        rh = elem_h;
        rw = elem_h * video_aspect;
    }

    float ox = (elem_w - rw) * 0.5f;
    float oy = (elem_h - rh) * 0.5f;

    // Build a textured quad (2 triangles)
    Rml::Vertex vertices[4];
    for (auto& v : vertices)
        v.colour = Rml::ColourbPremultiplied(255, 255, 255, 255);

    vertices[0].position = {ox, oy};
    vertices[0].tex_coord = {texture_u0_, texture_v0_};

    vertices[1].position = {ox + rw, oy};
    vertices[1].tex_coord = {texture_u1_, texture_v0_};

    vertices[2].position = {ox + rw, oy + rh};
    vertices[2].tex_coord = {texture_u1_, texture_v1_};

    vertices[3].position = {ox, oy + rh};
    vertices[3].tex_coord = {texture_u0_, texture_v1_};

    int indices[6] = {0, 1, 2, 0, 2, 3};

    video_geom_ = ri->CompileGeometry({vertices, 4}, {indices, 6});
    geom_w_ = elem_w;
    geom_h_ = elem_h;
    geometry_uv_dirty_ = false;
}

void VideoElement::OnResize() {
    Rml::Element::OnResize();
    // Invalidate geometry so it gets rebuilt with new dimensions
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x != geom_w_ || size.y != geom_h_) {
        if (video_geom_) {
            auto* ri = Rml::GetRenderInterface();
            if (ri) { ri->ReleaseGeometry(video_geom_); video_geom_ = 0; }
        }
    }
}

void VideoElement::OnRender() {
	ZoneScopedN("VideoElement::OnRender");
    if (!has_frame_ || frame_width_ == 0 || frame_height_ == 0) return;

    auto* ri = Rml::GetRenderInterface();
    if (!ri) return;
    auto* ext_ri = static_cast<ExtendedRenderInterface*>(ri);

    // Rebuild geometry if element size changed
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (!video_geom_ || size.x != geom_w_ || size.y != geom_h_ || geometry_uv_dirty_)
        RebuildGeometry();
    if (!video_geom_) return;

    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);

    if (nv12_mode_) {
        // NV12 path: Y + interleaved UV, native hardware decoder format
        if (nv12_dirty_ && (nv12_native_mode_ || !nv12_y_.empty())) {
            const bool compatible = nv12_texture_ &&
                nv12_tex_w_ == frame_width_ && nv12_tex_h_ == frame_height_ &&
                nv12_texture_native_ == nv12_native_mode_;
            if (!compatible && nv12_texture_) {
                ext_ri->ReleaseNV12Texture(nv12_texture_);
                nv12_texture_ = 0;
            }

            if (nv12_native_mode_) {
                if (nv12_texture_) {
                    if (!ext_ri->UpdateNativeNV12Texture(nv12_texture_, nv12_native_resource_,
                            nv12_native_chroma_resource_, nv12_native_owner_,
                            nv12_native_ready_fence_, nv12_native_ready_value_,
                            nv12_native_resource_state_, nv12_native_rgba_, frame_width_, frame_height_)) {
                        ext_ri->ReleaseNV12Texture(nv12_texture_);
                        nv12_texture_ = 0;
                    }
                }
                if (!nv12_texture_) {
                    nv12_texture_ = ext_ri->GenerateNativeNV12Texture(
                        nv12_native_resource_, nv12_native_chroma_resource_,
                        nv12_native_owner_, nv12_native_ready_fence_,
                        nv12_native_ready_value_, nv12_native_resource_state_, nv12_native_rgba_,
                        frame_width_, frame_height_);
                }
            } else {
                if (nv12_texture_) {
                    ext_ri->UpdateNV12Texture(nv12_texture_,
                        nv12_y_.data(), nv12_y_stride_,
                        nv12_uv_.data(), nv12_uv_stride_,
                        frame_width_, frame_height_);
                } else {
                    nv12_texture_ = ext_ri->GenerateNV12Texture(
                        nv12_y_.data(), nv12_y_stride_,
                        nv12_uv_.data(), nv12_uv_stride_,
                        frame_width_, frame_height_);
                }
            }
            if (nv12_texture_) {
                nv12_tex_w_ = frame_width_;
                nv12_tex_h_ = frame_height_;
                nv12_texture_native_ = nv12_native_mode_;
                nv12_dirty_ = false;
            }
        }
        if (nv12_texture_)
            ext_ri->RenderNV12Geometry(video_geom_, offset, nv12_texture_);
    } else if (yuv_mode_) {
        // I420 path: 3 separate R8 textures
        if (yuv_dirty_ && !yuv_y_.empty()) {
            if (yuv_texture_ && yuv_tex_w_ == frame_width_ && yuv_tex_h_ == frame_height_) {
                ext_ri->UpdateYUVTexture(yuv_texture_,
                    yuv_y_.data(), yuv_y_stride_,
                    yuv_u_.data(), yuv_v_.data(), yuv_uv_stride_,
                    frame_width_, frame_height_);
            } else {
                if (yuv_texture_)
                    ext_ri->ReleaseYUVTexture(yuv_texture_);
                yuv_texture_ = ext_ri->GenerateYUVTexture(
                    yuv_y_.data(), yuv_y_stride_,
                    yuv_u_.data(), yuv_v_.data(), yuv_uv_stride_,
                    frame_width_, frame_height_);
                yuv_tex_w_ = frame_width_;
                yuv_tex_h_ = frame_height_;
            }
            yuv_dirty_ = false;
        }
        if (yuv_texture_)
            ext_ri->RenderYUVGeometry(video_geom_, offset, yuv_texture_);
    } else {
        // RGBA path (fallback)
        if (frame_data_.empty()) return;

        if (texture_dirty_) {
            Rml::Vector2i dims(static_cast<int>(frame_width_), static_cast<int>(frame_height_));
            Rml::Span<const Rml::byte> data{frame_data_.data(), frame_data_.size()};

            if (video_texture_ && texture_w_ == frame_width_ && texture_h_ == frame_height_) {
                ext_ri->UpdateTextureData(video_texture_, data, dims);
            } else {
                if (video_texture_) {
                    ri->ReleaseTexture(video_texture_);
                    video_texture_ = 0;
                }
                video_texture_ = ext_ri->GenerateDynamicTexture(data, dims);
                texture_w_ = frame_width_;
                texture_h_ = frame_height_;
            }
            texture_dirty_ = false;
        }
        if (video_texture_)
            ri->RenderGeometry(video_geom_, offset, video_texture_);
    }
}

} // namespace parties::client
