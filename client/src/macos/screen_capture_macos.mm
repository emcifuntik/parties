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
@property (nonatomic, assign) std::function<void(const float*, uint32_t)> onAudio;
@property (nonatomic, assign) std::function<void()> onClosed;
@end

@implementation PartiesCaptureOutput

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type == SCStreamOutputTypeScreen) {
        CVPixelBufferRef pixel_buf =
            CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pixel_buf) return;

        uint32_t w = (uint32_t)CVPixelBufferGetWidth(pixel_buf);
        uint32_t h = (uint32_t)CVPixelBufferGetHeight(pixel_buf);

        if (self.onFrame) {
            CFRetain(pixel_buf);
            self.onFrame(pixel_buf, w, h);
        }
    } else if (type == SCStreamOutputTypeAudio) {
        if (!self.onAudio) return;

        // Get the audio format to determine interleaved vs non-interleaved
        CMFormatDescriptionRef fmtDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (!fmtDesc) return;
        const AudioStreamBasicDescription* asbd =
            CMAudioFormatDescriptionGetStreamBasicDescription(fmtDesc);
        if (!asbd) return;

        CMItemCount numSamples = CMSampleBufferGetNumSamples(sampleBuffer);
        if (numSamples == 0) return;

        bool isNonInterleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        uint32_t channels = asbd->mChannelsPerFrame;

        // AudioBufferList has mBuffers[1] by default — not enough for
        // non-interleaved stereo which needs 2 AudioBuffer entries.
        // Allocate storage for up to 2 buffers on the stack.
        union {
            AudioBufferList abl;
            uint8_t storage[sizeof(AudioBufferList) + sizeof(AudioBuffer)];
        } ablUnion;
        memset(&ablUnion, 0, sizeof(ablUnion));

        CMBlockBufferRef blockOut = nullptr;
        size_t bufListSize = sizeof(ablUnion);
        OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, &bufListSize, &ablUnion.abl, sizeof(ablUnion),
            nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &blockOut);
        if (st != noErr) {
            NSLog(@"[ScreenCaptureMac] GetAudioBufferList failed: %d (bufListSize=%zu)",
                  (int)st, bufListSize);
            return;
        }

        AudioBufferList& abl = ablUnion.abl;
        uint32_t frame_count = (uint32_t)numSamples;

        if (isNonInterleaved && channels >= 2 && abl.mNumberBuffers >= 2) {
            // Planar: abl.mBuffers[0] = L, abl.mBuffers[1] = R — interleave them
            const float* left  = (const float*)abl.mBuffers[0].mData;
            const float* right = (const float*)abl.mBuffers[1].mData;
            std::vector<float> interleaved(frame_count * 2);
            for (uint32_t i = 0; i < frame_count; i++) {
                interleaved[i * 2]     = left[i];
                interleaved[i * 2 + 1] = right[i];
            }
            self.onAudio(interleaved.data(), frame_count);
        } else {
            // Already interleaved (or mono)
            const float* data = (const float*)abl.mBuffers[0].mData;
            self.onAudio(data, frame_count);
        }

        if (blockOut) CFRelease(blockOut);
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
    NSLog(@"[ScreenCaptureMac] didUpdateWithFilter (filter=%@, stream=%@)", filter, stream);
    if (self.onFilterSelected)
        self.onFilterSelected(filter);
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker
          didCancelForStream:(SCStream*)stream
{
    NSLog(@"[ScreenCaptureMac] didCancelForStream");
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

    // Nested class has implicit access to ScreenCaptureMac private members (C++11).
    static void start_with_filter(ScreenCaptureMac* cap, Impl* impl,
                                  SCContentFilter* filter, uint32_t target_fps,
                                  std::function<void(bool)> on_started);
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

void ScreenCaptureMac::Impl::start_with_filter(ScreenCaptureMac* cap,
                                                Impl* impl,
                                                SCContentFilter* filter,
                                                uint32_t target_fps,
                                                std::function<void(bool)> on_started)
{
    NSLog(@"[ScreenCaptureMac] start_with_filter: fps=%u", target_fps);

    // Wire callbacks into the delegate.
    impl->output.onFrame = [cap](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        cap->width_  = w;
        cap->height_ = h;
        if (cap->on_frame) cap->on_frame(buf, w, h);
        CFRelease(buf);
    };
    impl->output.onAudio = [cap](const float* samples, uint32_t frame_count) {
        if (cap->on_audio) cap->on_audio(samples, frame_count);
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

    // Audio capture — 48 kHz stereo float, exclude our own process audio
    cfg.capturesAudio                = YES;
    cfg.excludesCurrentProcessAudio  = YES;
    cfg.sampleRate                   = 48000;
    cfg.channelCount                 = 2;

    // Capture at full Retina pixel resolution (2x on HiDPI).
    // Without this, SCK defaults to point dimensions (half res on Retina).
    CGSize source_size = filter.contentRect.size;
    CGFloat scale      = filter.pointPixelScale;
    cfg.width  = (NSUInteger)(source_size.width  * scale);
    cfg.height = (NSUInteger)(source_size.height * scale);
    NSLog(@"[ScreenCaptureMac] capture resolution: %lux%lu (scale=%.1f)",
          (unsigned long)cfg.width, (unsigned long)cfg.height, scale);

    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:cfg
                                               delegate:impl->output];

    NSError* addErr = nil;
    [stream addStreamOutput:impl->output
                       type:SCStreamOutputTypeScreen
         sampleHandlerQueue:impl->queue
                      error:&addErr];
    if (addErr) {
        NSLog(@"[ScreenCaptureMac] addStreamOutput (screen): %@", addErr.localizedDescription);
        if (on_started) on_started(false);
        return;
    }

    // Add audio output on the same queue
    NSError* audioErr = nil;
    [stream addStreamOutput:impl->output
                       type:SCStreamOutputTypeAudio
         sampleHandlerQueue:impl->queue
                      error:&audioErr];
    if (audioErr) {
        NSLog(@"[ScreenCaptureMac] addStreamOutput (audio): %@", audioErr.localizedDescription);
        // Non-fatal — continue without audio
    }

    NSLog(@"[ScreenCaptureMac] starting capture...");
    [stream startCaptureWithCompletionHandler:^(NSError* startErr) {
        if (startErr) {
            NSLog(@"[ScreenCaptureMac] startCapture: %@", startErr.localizedDescription);
            if (on_started) on_started(false);
        } else {
            impl->stream    = stream;
            cap->capturing_ = true;
            NSLog(@"[ScreenCaptureMac] capture started OK");
            if (on_started) on_started(true);
        }
    }];
}

// ── pick_and_start ───────────────────────────────────────────────────────────

void ScreenCaptureMac::pick_and_start(uint32_t target_fps,
                                       std::function<void(bool)> on_started)
{
    if (capturing_) stop();

    ScreenCaptureMac* cap = this;

    if (@available(macOS 14.0, *)) {
        NSLog(@"[ScreenCaptureMac] Using SCContentSharingPicker (macOS 14+)");
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
            NSLog(@"[ScreenCaptureMac] onFilterSelected block called");
            // Remove observer (one-shot)
            [picker removeObserver:observer];
            cap->impl_->observer = nil;
            picker.active = NO;

            Impl::start_with_filter(cap, cap->impl_, filter, target_fps, on_started);
        };

        observer.onCancelled = ^{
            NSLog(@"[ScreenCaptureMac] onCancelled block called");
            [picker removeObserver:observer];
            cap->impl_->observer = nil;
            picker.active = NO;

            if (on_started) on_started(false);
        };

        [picker addObserver:observer];
        picker.active = YES;
        NSLog(@"[ScreenCaptureMac] Presenting picker...");
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

                Impl::start_with_filter(cap, cap->impl_, filter, target_fps, on_started);
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
