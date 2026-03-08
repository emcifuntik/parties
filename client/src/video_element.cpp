#include <client/video_element.h>

#include "dx12/RmlUi_Renderer_DX12.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Context.h>

#include <cstdio>
#include <cstring>
#include <parties/profiler.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace parties::client {

// ── VideoElement ────────────────────────────────────────────────────

VideoElement::VideoElement(const Rml::String& tag)
    : Rml::Element(tag) {}

VideoElement::~VideoElement() {
    ReleaseResources();
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

    // Store plane data — uploaded to GPU in OnRender (must happen on render thread)
    uint32_t half_w = width / 2;
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

void VideoElement::UpdateFrame(std::vector<uint8_t>&& rgba_data, uint32_t width, uint32_t height) {
    bool size_changed = (frame_width_ != width || frame_height_ != height);
    frame_width_  = width;
    frame_height_ = height;
    has_frame_ = true;
    yuv_mode_ = false;
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
    nv12_dirty_ = false;
    nv12_y_.clear();
    nv12_uv_.clear();
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
        static_cast<RenderInterface_DX12*>(ri)->ReleaseYUVTexture(yuv_texture_);
        yuv_texture_ = 0;
    }
    yuv_tex_w_ = yuv_tex_h_ = 0;

    if (nv12_texture_) {
        static_cast<RenderInterface_DX12*>(ri)->ReleaseNV12Texture(nv12_texture_);
        nv12_texture_ = 0;
    }
    nv12_tex_w_ = nv12_tex_h_ = 0;

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
    vertices[0].tex_coord = {0.0f, 0.0f};

    vertices[1].position = {ox + rw, oy};
    vertices[1].tex_coord = {1.0f, 0.0f};

    vertices[2].position = {ox + rw, oy + rh};
    vertices[2].tex_coord = {1.0f, 1.0f};

    vertices[3].position = {ox, oy + rh};
    vertices[3].tex_coord = {0.0f, 1.0f};

    int indices[6] = {0, 1, 2, 0, 2, 3};

    video_geom_ = ri->CompileGeometry({vertices, 4}, {indices, 6});
    geom_w_ = elem_w;
    geom_h_ = elem_h;
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
    auto* dx12_ri = static_cast<RenderInterface_DX12*>(ri);

    // Rebuild geometry if element size changed
    Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (!video_geom_ || size.x != geom_w_ || size.y != geom_h_)
        RebuildGeometry();
    if (!video_geom_) return;

    Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);

    if (nv12_mode_) {
        // NV12 path: Y + interleaved UV, native hardware decoder format
        if (nv12_dirty_ && !nv12_y_.empty()) {
            if (nv12_texture_ && nv12_tex_w_ == frame_width_ && nv12_tex_h_ == frame_height_) {
                dx12_ri->UpdateNV12Texture(nv12_texture_,
                    nv12_y_.data(), nv12_y_stride_,
                    nv12_uv_.data(), nv12_uv_stride_,
                    frame_width_, frame_height_);
            } else {
                if (nv12_texture_)
                    dx12_ri->ReleaseNV12Texture(nv12_texture_);
                nv12_texture_ = dx12_ri->GenerateNV12Texture(
                    nv12_y_.data(), nv12_y_stride_,
                    nv12_uv_.data(), nv12_uv_stride_,
                    frame_width_, frame_height_);
                nv12_tex_w_ = frame_width_;
                nv12_tex_h_ = frame_height_;
            }
            nv12_dirty_ = false;
        }
        if (nv12_texture_)
            dx12_ri->RenderNV12Geometry(video_geom_, offset, nv12_texture_);
    } else if (yuv_mode_) {
        // I420 path: 3 separate R8 textures
        if (yuv_dirty_ && !yuv_y_.empty()) {
            if (yuv_texture_ && yuv_tex_w_ == frame_width_ && yuv_tex_h_ == frame_height_) {
                dx12_ri->UpdateYUVTexture(yuv_texture_,
                    yuv_y_.data(), yuv_y_stride_,
                    yuv_u_.data(), yuv_v_.data(), yuv_uv_stride_,
                    frame_width_, frame_height_);
            } else {
                if (yuv_texture_)
                    dx12_ri->ReleaseYUVTexture(yuv_texture_);
                yuv_texture_ = dx12_ri->GenerateYUVTexture(
                    yuv_y_.data(), yuv_y_stride_,
                    yuv_u_.data(), yuv_v_.data(), yuv_uv_stride_,
                    frame_width_, frame_height_);
                yuv_tex_w_ = frame_width_;
                yuv_tex_h_ = frame_height_;
            }
            yuv_dirty_ = false;
        }
        if (yuv_texture_)
            dx12_ri->RenderYUVGeometry(video_geom_, offset, yuv_texture_);
    } else {
        // RGBA path (fallback)
        if (frame_data_.empty()) return;

        if (texture_dirty_) {
            Rml::Vector2i dims(static_cast<int>(frame_width_), static_cast<int>(frame_height_));
            Rml::Span<const Rml::byte> data{frame_data_.data(), frame_data_.size()};

            if (video_texture_ && texture_w_ == frame_width_ && texture_h_ == frame_height_) {
                dx12_ri->UpdateTextureData(video_texture_, data, dims);
            } else {
                if (video_texture_) {
                    ri->ReleaseTexture(video_texture_);
                    video_texture_ = 0;
                }
                video_texture_ = ri->GenerateTexture(data, dims);
                texture_w_ = frame_width_;
                texture_h_ = frame_height_;
            }
            texture_dirty_ = false;
        }
        if (video_texture_)
            ri->RenderGeometry(video_geom_, offset, video_texture_);
    }
}

// ── Instancer ───────────────────────────────────────────────────────

Rml::ElementPtr VideoElementInstancer::InstanceElement(
    Rml::Element* /*parent*/, const Rml::String& tag,
    const Rml::XMLAttributes& /*attributes*/)
{
    return Rml::ElementPtr(new VideoElement(tag));
}

void VideoElementInstancer::ReleaseElement(Rml::Element* element) {
    delete element;
}

} // namespace parties::client
