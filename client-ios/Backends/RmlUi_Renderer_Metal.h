#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#endif

/**
 * Metal render interface for RmlUi on iOS.
 *
 * Usage per frame (from MTKViewDelegate):
 *   renderer.BeginFrame(commandBuffer, renderPassDescriptor);
 *   rml_context->Render();
 *   renderer.EndFrame();   // encodes draw calls into commandBuffer
 */
class RenderInterface_Metal : public Rml::RenderInterface {
public:
#ifdef __OBJC__
    /// Initialize with an existing Metal device and MTKView (used to obtain pixel format).
    explicit RenderInterface_Metal(id<MTLDevice> device, MTKView* view);

    /// Call at the start of each frame with the current command buffer and render pass.
    void BeginFrame(id<MTLCommandBuffer> command_buffer, MTLRenderPassDescriptor* pass_descriptor);
#endif

    ~RenderInterface_Metal();

    /// Update the viewport dimensions (call when the drawable size changes).
    void SetViewport(int width, int height);

    /// Set the Y pixel offset into the framebuffer where the Metal viewport begins.
    /// Used on iPhone to push all rendering (including the debugger) below the Dynamic Island.
    void SetViewportTopOffset(int top);

    /// Call after context->Render() to commit all encoded draw commands.
    void EndFrame();

    // ---- Rml::RenderInterface ----

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;

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

private:
    struct Data;
    Data* m_data = nullptr;
};
