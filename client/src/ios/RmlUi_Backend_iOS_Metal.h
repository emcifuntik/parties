#pragma once

#ifdef __OBJC__
#import <MetalKit/MetalKit.h>
#endif

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

// iOS + Metal backend for RmlUi.
// Same API as the macOS Metal backend but uses SystemInterface_iOS.
namespace Backend {

#ifdef __OBJC__
bool Initialize(id<MTLDevice> device, MTKView* view);
void BeginFrame(id<MTLCommandBuffer> command_buffer, MTLRenderPassDescriptor* pass_descriptor);
#endif

void Shutdown();

Rml::SystemInterface* GetSystemInterface();
Rml::RenderInterface* GetRenderInterface();

void SetViewport(int width, int height);
void SetViewportTopOffset(int top);
void EndFrame();

} // namespace Backend
