#pragma once

#import <MetalKit/MetalKit.h>

#include <client/pip_geometry.h>
#include <client/rml_shutdown.h>

#include <functional>
#include <optional>
#include <string>

class ExtendedRenderInterface;
namespace parties::client { class VideoElement; }

struct MacPipActions {
    std::function<void()> return_to_main;
    std::function<void(float)> set_volume;
    std::function<void()> close;
    std::function<float()> volume;
    std::function<std::optional<parties::client::PipRect>()> load_geometry;
    std::function<void(const parties::client::PipRect&)> save_geometry;
};

@interface PartiesPipHost : NSObject <MTKViewDelegate>

- (instancetype)initWithDevice:(id<MTLDevice>)device actions:(MacPipActions)actions;

- (BOOL)showWithTitle:(const std::string&)title;
- (void)hide;
- (BOOL)isVisible;
- (parties::client::VideoElement*)videoSurface;
- (ExtendedRenderInterface*)renderer;

- (void)releaseDocuments;
- (void)destroyWindow:(const parties::client::RmlShutdownProof&)rmlGone;

@end
