#pragma once

// Forward declarations for Obj-C types in C++ context
#ifdef __OBJC__
#import <MetalKit/MetalKit.h>
#endif

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

/**
 * iOS + Metal backend for RmlUi.
 *
 * Usage:
 *   1. Call Backend::Initialize(device, view) once at startup.
 *   2. Call Rml::SetSystemInterface / SetRenderInterface / Rml::Initialise.
 *   3. Each frame (from MTKViewDelegate::drawInMTKView:):
 *        Backend::BeginFrame(commandBuffer, passDescriptor);
 *        rml_context->Update();
 *        rml_context->Render();
 *        Backend::EndFrame();
 *        [commandBuffer presentDrawable: view.currentDrawable];
 *        [commandBuffer commit];
 *   4. Call Backend::Shutdown() and Rml::Shutdown() on teardown.
 */
namespace Backend {

#ifdef __OBJC__
bool Initialize(id<MTLDevice> device, MTKView* view);
void BeginFrame(id<MTLCommandBuffer> command_buffer, MTLRenderPassDescriptor* pass_descriptor);
#endif

void Shutdown();

Rml::SystemInterface* GetSystemInterface();
Rml::RenderInterface* GetRenderInterface();

/// Call when the drawable size changes (e.g. rotation or window resize).
void SetViewport(int width, int height);

/// Push the entire render area down by 'top' physical pixels (Dynamic Island / status bar).
/// Must be called before each frame for the offset to take effect.
void SetViewportTopOffset(int top);

/// End the render pass (call after context->Render()).
void EndFrame();

} // namespace Backend
