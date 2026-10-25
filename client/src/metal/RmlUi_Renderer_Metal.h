#pragma once

#include "RmlUi_RenderInterface_Extended.h"
#include <RmlUi/Core/Types.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#endif

class RenderInterface_Metal : public ExtendedRenderInterface {
public:
#ifdef __OBJC__
    explicit RenderInterface_Metal(id<MTLDevice> device, MTKView* view);

    void BeginFrame(id<MTLCommandBuffer> command_buffer, MTLRenderPassDescriptor* pass_descriptor);
#endif

    ~RenderInterface_Metal();

    explicit operator bool() const override;

    bool IsFrameActive() const override;

    void SetViewport(int width, int height) override;

    void SetViewportTopOffset(int top);

    void EndFrame() override;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;
    void UpdateGeometryVertices(Rml::CompiledGeometryHandle geometry,
                                Rml::Span<const Rml::Vertex> vertices) override;
    void UpdateTextureData(Rml::TextureHandle texture_handle,
                           Rml::Span<const Rml::byte> source_data,
                           Rml::Vector2i source_dimensions) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation,
                          Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name,
                                            const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader,
                      Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

    uintptr_t GenerateNV12Texture(
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height) override;
    void UpdateNV12Texture(uintptr_t handle,
        const uint8_t* y_data, uint32_t y_stride,
        const uint8_t* uv_data, uint32_t uv_stride,
        uint32_t width, uint32_t height) override;
    void ReleaseNV12Texture(uintptr_t handle) override;
    void RenderNV12Geometry(Rml::CompiledGeometryHandle geometry,
        Rml::Vector2f translation, uintptr_t nv12_handle) override;

private:
    struct Data;
    Data* m_data = nullptr;

};
