#pragma once

#ifdef __OBJC__
#import <MetalKit/MetalKit.h>
#endif

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

class RenderInterface_Metal;

/**
 * macOS + Metal backend for RmlUi.
 *
 * Usage:
 *   1. Backend::Initialize(device, view) once at startup.
 *   2. Rml::SetSystemInterface / SetRenderInterface / Rml::Initialise.
 *   3. Each frame (MTKViewDelegate::drawInMTKView:):
 *        Backend::BeginFrame(commandBuffer, passDescriptor);
 *        rml_context->Update();
 *        rml_context->Render();
 *        Backend::EndFrame();
 *        [commandBuffer presentDrawable:view.currentDrawable];
 *        [commandBuffer commit];
 *   4. Backend::Shutdown() then Rml::Shutdown() on teardown.
 */
namespace Backend {

#ifdef __OBJC__
bool Initialize(id<MTLDevice> device, MTKView* view);
void BeginFrame(id<MTLCommandBuffer> command_buffer,
                MTLRenderPassDescriptor* pass_descriptor);
#endif

void Shutdown();

Rml::SystemInterface* GetSystemInterface();
Rml::RenderInterface* GetRenderInterface();

/// Concrete Metal render interface for platform-specific rendering extensions.
RenderInterface_Metal* GetMetalRenderInterface();

void SetViewport(int width, int height);

void EndFrame();

} // namespace Backend
