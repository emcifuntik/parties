#pragma once

#import <AVKit/AVKit.h>
#import <UIKit/UIKit.h>

#include <functional>

@interface PartiesStreamPictureInPicture : NSObject <AVPictureInPictureControllerDelegate,
                                                     AVPictureInPictureSampleBufferPlaybackDelegate>

+ (BOOL)isSupported;

- (instancetype)initWithHostView:(UIView*)hostView;

// The restore completion must be called with YES once the stream is visible again.
- (void)setOnStarted:(std::function<void()>)onStarted;
- (void)setOnStopped:(std::function<void()>)onStopped;
- (void)setOnRestore:(std::function<void(void (^)(BOOL))>)onRestore;

- (void)enqueuePixelBuffer:(CVPixelBufferRef)pixelBuffer;
- (void)flush;

- (void)setInlineFrame:(CGRect)frame exclude:(CGRect)exclude;
- (void)hideInline;

- (void)start;
- (void)stop;
- (BOOL)isActive;

@end
