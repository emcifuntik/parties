#pragma once

#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <CoreVideo/CoreVideo.h>

// Presents a fullscreen landscape view that renders an incoming NV12 video
// stream via Metal.  Tap anywhere to dismiss.
//
// Call -setPixelBuffer: from any thread whenever a new decoded frame arrives.
// The view will render it on the next draw cycle.

@interface ScreenShareViewController : UIViewController <MTKViewDelegate>

// Called on the main thread after the view controller is dismissed.
@property (nonatomic, copy) dispatch_block_t onDismissed;

// Thread-safe: deliver a decoded CVPixelBufferRef here.
// The receiver retains the buffer internally and releases it after use.
- (void)setPixelBuffer:(CVPixelBufferRef)buffer;

@end
