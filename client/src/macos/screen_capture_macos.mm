// macOS screen capture — ScreenCaptureKit backend.
//
// macOS 14+: uses SCContentSharingPicker for system-native target selection.
// macOS 12.3-13: falls back to capturing the main display.

#include "screen_capture_macos.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

// ── SCStreamOutput delegate ───────────────────────────────────────────────────

@interface PartiesCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) std::function<void(CVPixelBufferRef, uint32_t, uint32_t)> onFrame;
@property (nonatomic, assign) std::function<void()> onClosed;
@end

@implementation PartiesCaptureOutput

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen) return;

    CVPixelBufferRef pixel_buf =
        CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixel_buf) return;

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(pixel_buf);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(pixel_buf);

    if (self.onFrame) {
        CFRetain(pixel_buf);
        self.onFrame(pixel_buf, w, h);
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    if (error)
        NSLog(@"[ScreenCaptureMac] Stream stopped: %@", error.localizedDescription);
    if (self.onClosed)
        self.onClosed();
}

@end

// ── SCContentSharingPicker observer (macOS 14+) ─────────────────────────────

API_AVAILABLE(macos(14.0))
@interface PartiesPickerObserver : NSObject <SCContentSharingPickerObserver>
@property (nonatomic, copy) void (^onFilterSelected)(SCContentFilter* filter);
@property (nonatomic, copy) void (^onCancelled)(void);
@end

@implementation PartiesPickerObserver

- (void)contentSharingPicker:(SCContentSharingPicker*)picker
        didUpdateWithFilter:(SCContentFilter*)filter
                  forStream:(SCStream*)stream
{
    if (self.onFilterSelected)
        self.onFilterSelected(filter);
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker
          didCancelForStream:(SCStream*)stream
{
    if (self.onCancelled)
        self.onCancelled();
}

- (void)contentSharingPickerStartDidFailWithError:(NSError*)error
{
    NSLog(@"[ScreenCaptureMac] Picker failed: %@", error.localizedDescription);
    if (self.onCancelled)
        self.onCancelled();
}

@end

// ── Impl ─────────────────────────────────────────────────────────────────────

namespace parties::client {

struct ScreenCaptureMac::Impl {
    SCStream*             stream   = nullptr;
    PartiesCaptureOutput* output   = nullptr;
    dispatch_queue_t      queue    = nullptr;
    id                    observer = nil;  // PartiesPickerObserver (macOS 14+)
};

ScreenCaptureMac::ScreenCaptureMac()
    : impl_(new Impl())
{
    impl_->queue = dispatch_queue_create("com.parties.screencapture",
                                         DISPATCH_QUEUE_SERIAL);
    impl_->output = [[PartiesCaptureOutput alloc] init];
}

ScreenCaptureMac::~ScreenCaptureMac()
{
    stop();
    if (@available(macOS 14.0, *)) {
        if (impl_->observer) {
            auto* picker = [SCContentSharingPicker sharedPicker];
            [picker removeObserver:(PartiesPickerObserver*)impl_->observer];
            impl_->observer = nil;
        }
    }
    if (impl_->queue) {
        dispatch_release(impl_->queue);
        impl_->queue = nullptr;
    }
    delete impl_;
}

// ── Helper: start capture with a given filter ────────────────────────────────

static bool StartWithFilter(ScreenCaptureMac* cap,
                             ScreenCaptureMac::Impl* impl,
                             SCContentFilter* filter,
                             uint32_t target_fps)
{
    // Wire callbacks into the delegate.
    impl->output.onFrame = [cap](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        cap->width_  = w;
        cap->height_ = h;
        if (cap->on_frame) cap->on_frame(buf, w, h);
        CFRelease(buf);
    };
    impl->output.onClosed = [cap]() {
        cap->capturing_ = false;
        if (cap->on_closed) cap->on_closed();
    };

    SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
    cfg.minimumFrameInterval =
        CMTimeMake(1, (int32_t)(target_fps > 0 ? target_fps : 60));
    cfg.pixelFormat          = kCVPixelFormatType_32BGRA;
    cfg.colorSpaceName       = kCGColorSpaceSRGB;
    cfg.showsCursor          = NO;

    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:cfg
                                               delegate:impl->output];

    NSError* addErr = nil;
    [stream addStreamOutput:impl->output
                       type:SCStreamOutputTypeScreen
         sampleHandlerQueue:impl->queue
                      error:&addErr];
    if (addErr) {
        NSLog(@"[ScreenCaptureMac] addStreamOutput: %@", addErr.localizedDescription);
        return false;
    }

    __block bool success = false;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [stream startCaptureWithCompletionHandler:^(NSError* startErr) {
        if (startErr) {
            NSLog(@"[ScreenCaptureMac] startCapture: %@", startErr.localizedDescription);
        } else {
            impl->stream    = stream;
            cap->capturing_ = true;
            success          = true;
        }
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return success;
}

// ── pick_and_start ───────────────────────────────────────────────────────────

void ScreenCaptureMac::pick_and_start(uint32_t target_fps,
                                       std::function<void(bool)> on_started)
{
    if (capturing_) stop();

    ScreenCaptureMac* cap = this;

    if (@available(macOS 14.0, *)) {
        // Use system SCContentSharingPicker
        auto* picker = [SCContentSharingPicker sharedPicker];

        SCContentSharingPickerConfiguration* config =
            [[SCContentSharingPickerConfiguration alloc] init];
        config.allowedPickerModes =
            SCContentSharingPickerModeSingleWindow |
            SCContentSharingPickerModeSingleDisplay;
        picker.defaultConfiguration = config;

        auto* observer = [[PartiesPickerObserver alloc] init];
        impl_->observer = observer;

        observer.onFilterSelected = ^(SCContentFilter* filter) {
            // Remove observer (one-shot)
            [picker removeObserver:observer];
            cap->impl_->observer = nil;
            picker.active = NO;

            bool ok = StartWithFilter(cap, cap->impl_, filter, target_fps);
            if (on_started) on_started(ok);
        };

        observer.onCancelled = ^{
            [picker removeObserver:observer];
            cap->impl_->observer = nil;
            picker.active = NO;

            if (on_started) on_started(false);
        };

        [picker addObserver:observer];
        picker.active = YES;
        [picker present];

    } else {
        // Fallback (macOS 12.3-13): capture main display
        [SCShareableContent
            getShareableContentWithCompletionHandler:
            ^(SCShareableContent* content, NSError* error) {
                if (error || content.displays.count == 0) {
                    NSLog(@"[ScreenCaptureMac] Fallback: no displays available");
                    if (on_started) on_started(false);
                    return;
                }

                SCDisplay* mainDisplay = content.displays.firstObject;
                SCContentFilter* filter = [[SCContentFilter alloc]
                    initWithDisplay:mainDisplay excludingWindows:@[]];

                bool ok = StartWithFilter(cap, cap->impl_, filter, target_fps);
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (on_started) on_started(ok);
                });
            }];
    }
}

// ── stop ──────────────────────────────────────────────────────────────────────

void ScreenCaptureMac::stop()
{
    if (!impl_->stream) return;

    SCStream* stream = impl_->stream;
    impl_->stream    = nullptr;
    capturing_       = false;

    [stream stopCaptureWithCompletionHandler:^(NSError* err) {
        if (err)
            NSLog(@"[ScreenCaptureMac] stopCapture: %@", err.localizedDescription);
    }];
}

} // namespace parties::client
