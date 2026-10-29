#pragma once

#import <MetalKit/MetalKit.h>

#include <client/pip_geometry.h>

#include <functional>
#include <optional>
#include <string>

class ExtendedRenderInterface;
namespace parties::client { class VideoElement; }

// What the macOS PiP host asks of the application.
struct MacPipActions {
    std::function<void()> return_to_main;
    std::function<void(float)> set_volume;   // stream playback volume
    std::function<void()> close;
    std::function<float()> volume;
    std::function<std::optional<parties::client::PipRect>()> load_geometry;
    std::function<void(const parties::client::PipRect&)> save_geometry;
};

// macOS picture-in-picture presenter: a PartiesPipPanelController hosting its
// own MTKView, Metal render interface, RmlUi context, ui/pip.rml document and
// PipWindowModel. Created on first show and kept until shutdown, because
// RmlUi retains a render manager for every render interface until
// Rml::Shutdown. Main thread only.
@interface PartiesPipHost : NSObject <MTKViewDelegate>

- (instancetype)initWithDevice:(id<MTLDevice>)device actions:(MacPipActions)actions;

- (BOOL)showWithTitle:(const std::string&)title;
- (void)hide;
- (BOOL)isVisible;
// The PiP video surface, or null before the first show.
- (parties::client::VideoElement*)videoSurface;
- (ExtendedRenderInterface*)renderer;

// Before Rml::Shutdown: unload the document and remove the context.
- (void)prepareShutdown;
// After Rml::Shutdown: release the renderer and the panel.
- (void)shutdown;

@end
