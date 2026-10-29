#pragma once

#import <AVKit/AVKit.h>
#import <UIKit/UIKit.h>

#include <functional>

// The iOS surface of the watched stream, inline and in system
// picture-in-picture. Decoded CVPixelBuffers are enqueued (never copied or
// re-encoded) into one AVSampleBufferDisplayLayer that sits over the stream's
// grid cell; AVPictureInPictureController lifts that same layer into the
// system PiP window, which is what allows PiP to start automatically when the
// app goes to the background. Main thread only.
@interface PartiesStreamPictureInPicture : NSObject <AVPictureInPictureControllerDelegate,
                                                     AVPictureInPictureSampleBufferPlaybackDelegate>

+ (BOOL)isSupported;

// Adds the inline surface view to `hostView` (above its Metal content; it
// never receives touches).
- (instancetype)initWithHostView:(UIView*)hostView;

// Called when the system starts PiP (button or automatic), when PiP stops for
// any reason, and when the user taps the system restore button. The restore
// completion must be called with YES once the stream is visible again.
- (void)setOnStarted:(std::function<void()>)onStarted;
- (void)setOnStopped:(std::function<void()>)onStopped;
- (void)setOnRestore:(std::function<void(void (^)(BOOL))>)onRestore;

// Display a decoded frame. The sample buffer retains `pixelBuffer`.
- (void)enqueuePixelBuffer:(CVPixelBufferRef)pixelBuffer;
// Drop every queued frame and the displayed image (stream gone).
- (void)flush;

// Place the inline surface over the stream cell (points, in the host view).
// `exclude` is an overlay rect (or CGRectNull) the video must not cover.
- (void)setInlineFrame:(CGRect)frame exclude:(CGRect)exclude;
- (void)hideInline;

- (void)start;   // PiP button
- (void)stop;    // PiP closed by the app (stream ended, stop watching, ...)
- (BOOL)isActive;

@end
