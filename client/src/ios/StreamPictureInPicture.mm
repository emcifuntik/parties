#import "StreamPictureInPicture.h"

#import <CoreMedia/CoreMedia.h>
#import <QuartzCore/QuartzCore.h>

#include <utility>

// A view whose backing layer is the display layer, so it follows the frame.
@interface PartiesStreamSurfaceView : UIView
@end

@implementation PartiesStreamSurfaceView
+ (Class)layerClass { return [AVSampleBufferDisplayLayer class]; }
@end

@implementation PartiesStreamPictureInPicture {
    PartiesStreamSurfaceView* _surface;
    AVSampleBufferDisplayLayer* _layer;      // _surface.layer, not owned separately
    AVPictureInPictureController* _controller;
    CMVideoFormatDescriptionRef _format;
    CAShapeLayer* _mask;
    std::function<void()> _onStarted;
    std::function<void()> _onStopped;
    std::function<void(void (^)(BOOL))> _onRestore;
    BOOL _startRequested;
    BOOL _active;          // between willStart and didStop
    BOOL _restoring;
}

+ (BOOL)isSupported
{
    return [AVPictureInPictureController isPictureInPictureSupported];
}

- (instancetype)initWithHostView:(UIView*)hostView
{
    self = [super init];
    if (!self)
        return nil;

    _surface = [[PartiesStreamSurfaceView alloc] initWithFrame:CGRectZero];
    _surface.userInteractionEnabled = NO;   // RmlUi keeps all touches
    _surface.hidden = YES;
    _surface.backgroundColor = UIColor.clearColor;
    _layer = (AVSampleBufferDisplayLayer*)_surface.layer;
    _layer.videoGravity = AVLayerVideoGravityResizeAspect;
    [hostView addSubview:_surface];

    AVPictureInPictureControllerContentSource* source =
        [[AVPictureInPictureControllerContentSource alloc] initWithSampleBufferDisplayLayer:_layer
                                                                             playbackDelegate:self];
    _controller = [[AVPictureInPictureController alloc] initWithContentSource:source];
    [source release];
    _controller.delegate = self;
    // The watched stream is the user's primary content: leaving the app while
    // it plays inline moves it into PiP.
    _controller.canStartPictureInPictureAutomaticallyFromInline = YES;
    // A live stream cannot seek or skip.
    _controller.requiresLinearPlayback = YES;
    return self;
}

- (void)setOnStarted:(std::function<void()>)onStarted { _onStarted = std::move(onStarted); }
- (void)setOnStopped:(std::function<void()>)onStopped { _onStopped = std::move(onStopped); }
- (void)setOnRestore:(std::function<void(void (^)(BOOL))>)onRestore { _onRestore = std::move(onRestore); }

- (void)enqueuePixelBuffer:(CVPixelBufferRef)pixelBuffer
{
    if (!pixelBuffer) return;
    if (!_format || !CMVideoFormatDescriptionMatchesImageBuffer(_format, pixelBuffer)) {
        if (_format) CFRelease(_format);
        _format = nullptr;
        if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &_format) != noErr)
            return;
    }

    // Live video: show each frame as soon as it is enqueued, without a timebase.
    CMSampleTimingInfo timing{kCMTimeInvalid, CMClockGetTime(CMClockGetHostTimeClock()), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    if (CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixelBuffer, _format,
                                                 &timing, &sample) != noErr || !sample)
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, YES);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto* attachment = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(attachment, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    }

    // A failed layer (e.g. after a media services reset) only recovers on flush.
    if (_layer.status == AVQueuedSampleBufferRenderingStatusFailed)
        [_layer flush];
    [_layer enqueueSampleBuffer:sample];
    CFRelease(sample);

    // A PiP request made before the first frame starts once PiP is possible.
    if (_startRequested && !_active && _controller.isPictureInPicturePossible) {
        _startRequested = NO;
        [_controller startPictureInPicture];
    }
}

- (void)flush
{
    _startRequested = NO;
    [_layer flushAndRemoveImage];
}

- (void)setInlineFrame:(CGRect)frame exclude:(CGRect)exclude
{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _surface.frame = frame;
    // While the system shows the layer in PiP the cell shows a placeholder.
    _surface.hidden = _active || CGRectIsEmpty(frame);

    const CGRect overlap = CGRectIntersection(frame, exclude);
    if (CGRectIsNull(exclude) || CGRectIsEmpty(overlap)) {
        _surface.layer.mask = nil;
    } else {
        // The fullscreen call dock floats over the video: cut it out so the
        // RmlUi controls underneath stay visible.
        if (!_mask) _mask = [[CAShapeLayer alloc] init];
        UIBezierPath* path = [UIBezierPath bezierPathWithRect:_surface.bounds];
        [path appendPath:[UIBezierPath bezierPathWithRect:
            CGRectOffset(overlap, -frame.origin.x, -frame.origin.y)]];
        _mask.fillRule = kCAFillRuleEvenOdd;
        _mask.path = path.CGPath;
        _mask.frame = _surface.bounds;
        _surface.layer.mask = _mask;
    }
    [CATransaction commit];
}

- (void)hideInline
{
    _surface.hidden = YES;
}

- (void)start
{
    if (_active) return;
    if (_controller.isPictureInPicturePossible)
        [_controller startPictureInPicture];
    else
        _startRequested = YES;   // retried when the next frame arrives
}

- (void)stop
{
    _startRequested = NO;
    // During a restore the system is already stopping PiP.
    if (_active && !_restoring)
        [_controller stopPictureInPicture];
}

- (BOOL)isActive
{
    return _active;
}

// ── AVPictureInPictureControllerDelegate ──────────────────────────────────────

- (void)pictureInPictureControllerWillStartPictureInPicture:(AVPictureInPictureController*)controller
{
    _active = YES;
    _startRequested = NO;
    if (_onStarted) _onStarted();
}

- (void)pictureInPictureController:(AVPictureInPictureController*)controller
    failedToStartPictureInPictureWithError:(NSError*)error
{
    NSLog(@"[Parties] Picture-in-picture failed to start: %@", error);
    _active = NO;
    if (_onStopped) _onStopped();
}

- (void)pictureInPictureController:(AVPictureInPictureController*)controller
    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler:(void (^)(BOOL))completionHandler
{
    _restoring = YES;
    if (_onRestore) _onRestore(completionHandler);
    else completionHandler(YES);
}

- (void)pictureInPictureControllerDidStopPictureInPicture:(AVPictureInPictureController*)controller
{
    _active = NO;
    _restoring = NO;
    _surface.hidden = CGRectIsEmpty(_surface.frame);
    if (_onStopped) _onStopped();
}

// ── AVPictureInPictureSampleBufferPlaybackDelegate (live content) ─────────────

- (void)pictureInPictureController:(AVPictureInPictureController*)controller setPlaying:(BOOL)playing
{
    // A live stream cannot be paused; the system shows no pause control for it.
}

- (CMTimeRange)pictureInPictureControllerTimeRangeForPlayback:(AVPictureInPictureController*)controller
{
    // An infinite range marks the content as live.
    return CMTimeRangeMake(kCMTimeNegativeInfinity, kCMTimePositiveInfinity);
}

- (BOOL)pictureInPictureControllerIsPlaybackPaused:(AVPictureInPictureController*)controller
{
    return NO;
}

- (void)pictureInPictureController:(AVPictureInPictureController*)controller
         didTransitionToRenderSize:(CMVideoDimensions)newRenderSize
{
}

- (void)pictureInPictureController:(AVPictureInPictureController*)controller
                    skipByInterval:(CMTime)skipInterval
                 completionHandler:(void (^)(void))completionHandler
{
    completionHandler();
}

- (void)dealloc
{
    _controller.delegate = nil;
    [_controller release];
    [_surface removeFromSuperview];
    [_surface release];
    [_mask release];
    if (_format) CFRelease(_format);
    [super dealloc];
}

@end
