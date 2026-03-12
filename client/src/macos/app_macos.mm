// macOS application — entry point, window, render loop, and app logic.
//
// Mirrors the role of PartiesViewController.mm on iOS, using AppKit instead
// of UIKit.  NSWindow + MTKView host the Metal-backed RmlUI context.

#import "PartiesAppDelegate.h"
#import "screen_capture_macos.h"
#import <encdec/apple/video_encoder_macos.h>

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

// RmlUi Metal backend
#import "RmlUi_Backend_macOS_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#ifdef RMLUI_DEBUG
#include <RmlUi/Debugger.h>
#endif

// Parties protocol
#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/serialization.h>
#include <parties/audio_common.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/video_common.h>

// Shared client code
#include <parties/quic_common.h>
#include <client/app_core.h>
#include <client/sound_player.h>
#include <client/rmlui_backend.h>
#include <client/video_element.h>

#include <encdec/apple/VideoDecoderIOS.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdio>

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

// ── Key mapping — NSEvent key codes → RmlUi ──────────────────────────────────

static Rml::Input::KeyIdentifier macos_key_to_rml(unsigned short keyCode)
{
    switch (keyCode) {
    case 0x00: return Rml::Input::KI_A;
    case 0x0B: return Rml::Input::KI_B;
    case 0x08: return Rml::Input::KI_C;
    case 0x02: return Rml::Input::KI_D;
    case 0x0E: return Rml::Input::KI_E;
    case 0x03: return Rml::Input::KI_F;
    case 0x05: return Rml::Input::KI_G;
    case 0x04: return Rml::Input::KI_H;
    case 0x22: return Rml::Input::KI_I;
    case 0x26: return Rml::Input::KI_J;
    case 0x28: return Rml::Input::KI_K;
    case 0x25: return Rml::Input::KI_L;
    case 0x2E: return Rml::Input::KI_M;
    case 0x2D: return Rml::Input::KI_N;
    case 0x1F: return Rml::Input::KI_O;
    case 0x23: return Rml::Input::KI_P;
    case 0x0C: return Rml::Input::KI_Q;
    case 0x0F: return Rml::Input::KI_R;
    case 0x01: return Rml::Input::KI_S;
    case 0x11: return Rml::Input::KI_T;
    case 0x20: return Rml::Input::KI_U;
    case 0x09: return Rml::Input::KI_V;
    case 0x0D: return Rml::Input::KI_W;
    case 0x07: return Rml::Input::KI_X;
    case 0x10: return Rml::Input::KI_Y;
    case 0x06: return Rml::Input::KI_Z;
    case 0x1D: return Rml::Input::KI_0;
    case 0x12: return Rml::Input::KI_1;
    case 0x13: return Rml::Input::KI_2;
    case 0x14: return Rml::Input::KI_3;
    case 0x15: return Rml::Input::KI_4;
    case 0x17: return Rml::Input::KI_5;
    case 0x16: return Rml::Input::KI_6;
    case 0x1A: return Rml::Input::KI_7;
    case 0x1C: return Rml::Input::KI_8;
    case 0x19: return Rml::Input::KI_9;
    case 0x24: return Rml::Input::KI_RETURN;
    case 0x35: return Rml::Input::KI_ESCAPE;
    case 0x33: return Rml::Input::KI_BACK;
    case 0x30: return Rml::Input::KI_TAB;
    case 0x31: return Rml::Input::KI_SPACE;
    case 0x7B: return Rml::Input::KI_LEFT;
    case 0x7C: return Rml::Input::KI_RIGHT;
    case 0x7D: return Rml::Input::KI_DOWN;
    case 0x7E: return Rml::Input::KI_UP;
    case 0x75: return Rml::Input::KI_DELETE;
    case 0x73: return Rml::Input::KI_HOME;
    case 0x77: return Rml::Input::KI_END;
    case 0x74: return Rml::Input::KI_PRIOR;   // Page Up
    case 0x79: return Rml::Input::KI_NEXT;    // Page Down
    default:   return Rml::Input::KI_UNKNOWN;
    }
}

static int macos_modifiers_to_rml(NSEventModifierFlags flags)
{
    int mods = 0;
    if (flags & NSEventModifierFlagShift)   mods |= Rml::Input::KM_SHIFT;
    if (flags & NSEventModifierFlagControl) mods |= Rml::Input::KM_CTRL;
    if (flags & NSEventModifierFlagOption)  mods |= Rml::Input::KM_ALT;
    return mods;
}

// ── PartiesView — MTKView subclass that forwards input to RmlUi ───────────────

@interface PartiesView : MTKView
@property (nonatomic, assign) Rml::Context* rmlContext;
@end

@implementation PartiesView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (Rml::Vector2f)rmlPoint:(NSEvent*)event
{
    NSPoint pt    = [self convertPoint:event.locationInWindow fromView:nil];
    float   scale = (float)self.window.backingScaleFactor;
    return { (float)pt.x * scale,
             (float)(self.bounds.size.height - pt.y) * scale };
}

- (void)mouseMoved:(NSEvent*)event
{
    if (!_rmlContext) return;
    auto p = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)p.x, (int)p.y,
                                   macos_modifiers_to_rml(event.modifierFlags));
}
- (void)mouseDragged:(NSEvent*)event   { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)e  { [self mouseMoved:e]; }

- (void)mouseDown:(NSEvent*)event
{
    if (!_rmlContext) return;
    auto p = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)p.x, (int)p.y, 0);
    _rmlContext->ProcessMouseButtonDown(0, macos_modifiers_to_rml(event.modifierFlags));
}
- (void)mouseUp:(NSEvent*)event
{
    if (!_rmlContext) return;
    _rmlContext->ProcessMouseButtonUp(0, macos_modifiers_to_rml(event.modifierFlags));
}
- (void)rightMouseDown:(NSEvent*)event
{
    if (!_rmlContext) return;
    auto p = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)p.x, (int)p.y, 0);
    _rmlContext->ProcessMouseButtonDown(1, macos_modifiers_to_rml(event.modifierFlags));
}
- (void)rightMouseUp:(NSEvent*)event
{
    if (!_rmlContext) return;
    _rmlContext->ProcessMouseButtonUp(1, macos_modifiers_to_rml(event.modifierFlags));
}

- (void)scrollWheel:(NSEvent*)event
{
    if (!_rmlContext) return;
    _rmlContext->ProcessMouseWheel(
        Rml::Vector2f(-(float)event.scrollingDeltaX/100.f,
                      -(float)event.scrollingDeltaY/100.f),
        macos_modifiers_to_rml(event.modifierFlags));
}

- (void)keyDown:(NSEvent*)event
{
    if (!_rmlContext) return;
    int mods = macos_modifiers_to_rml(event.modifierFlags);
    _rmlContext->ProcessKeyDown(macos_key_to_rml(event.keyCode), mods);

    NSString* chars = event.characters;
    if (chars.length > 0) {
        for (NSUInteger i = 0; i < chars.length; i++) {
            unichar c = [chars characterAtIndex:i];
            if (c >= 32 && c != 127)
                _rmlContext->ProcessTextInput((Rml::Character)c);
        }
    }
}
- (void)keyUp:(NSEvent*)event
{
    if (!_rmlContext) return;
    _rmlContext->ProcessKeyUp(macos_key_to_rml(event.keyCode),
                               macos_modifiers_to_rml(event.modifierFlags));
}

@end

// ── PartiesViewController — drives render loop and app logic ──────────────────

@interface PartiesViewController : NSViewController <MTKViewDelegate>
// Called by the app delegate before quic_cleanup() to close all MsQuic handles.
- (void)shutdown;
@end

@implementation PartiesViewController {
    PartiesView*          _metalView;
    id<MTLCommandQueue>   _commandQueue;
    Rml::Context*         _rmlContext;
    Rml::ElementDocument* _doc;

    // Embedded file interface (must outlive RmlUi)
    EmbeddedFileInterface _fileInterface;

    // Video element instancer
    VideoElementInstancer _videoInstancer;

    // AppCore — all shared connection/audio/model logic
    AppCore _core;

    // Sound effects
    SoundPlayer _soundPlayer;

    // Screen share — sender (macOS-specific)
    std::unique_ptr<ScreenCaptureMac> _capturer;
    std::unique_ptr<VideoEncoderMac>  _encoder;
    bool                              _encoderReady;
    bool                              _needsKeyframe;

    // Screen share — receiver (macOS-specific VideoToolbox decoder)
    std::unique_ptr<VideoDecoderIOS>  _decoder;
}

// ── View setup ────────────────────────────────────────────────────────────────

- (void)loadView
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSAssert(device, @"No Metal device found");

    _metalView = [[PartiesView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)
                                             device:device];
    _metalView.delegate                = self;
    _metalView.colorPixelFormat        = MTLPixelFormatBGRA8Unorm;
    _metalView.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    _metalView.clearColor              = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
    _metalView.preferredFramesPerSecond = 60;
    _metalView.paused                  = NO;
    _metalView.enableSetNeedsDisplay   = NO;
    self.view = _metalView;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    id<MTLDevice> device = _metalView.device;

    // ── Metal + RmlUi ─────────────────────────────────────────────────────
    _commandQueue = [device newCommandQueue];
    Backend::Initialize(device, _metalView);

    Rml::SetFileInterface(&_fileInterface);
    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());

    Rml::Initialise();

    Rml::Factory::RegisterElementInstancer("video_frame", &_videoInstancer);

    CGSize physical = _metalView.drawableSize;
    float  dpRatio  = (float)[[NSScreen mainScreen] backingScaleFactor];

    Backend::SetViewport((int)physical.width, (int)physical.height);

    _rmlContext = Rml::CreateContext("main",
        Rml::Vector2i((int)physical.width, (int)physical.height));
    _rmlContext->SetDensityIndependentPixelRatio(dpRatio);
    _metalView.rmlContext = _rmlContext;

#ifdef RMLUI_DEBUG
    Rml::Debugger::Initialise(_rmlContext);
#endif

    // ── Fonts ─────────────────────────────────────────────────────────────
    Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf");

    // ── App state ─────────────────────────────────────────────────────────
    _encoderReady   = false;
    _needsKeyframe  = false;

    _soundPlayer.init();

    // ── Settings path ─────────────────────────────────────────────────────
    NSString* appSupport = [NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
    NSString* dir = [appSupport stringByAppendingPathComponent:@"Parties"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
    NSString* dbPath = [dir stringByAppendingPathComponent:@"parties.db"];

    // ── Build PlatformBridge ──────────────────────────────────────────────
    PartiesViewController* bself = self;

    PlatformBridge bridge;

    bridge.copy_to_clipboard = [](const std::string& text) {
        NSString* ns = [NSString stringWithUTF8String:text.c_str()];
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:ns forType:NSPasteboardTypeString];
    };

    bridge.play_sound = [bself](SoundPlayer::Effect e) {
        bself->_soundPlayer.play(e);
    };

    bridge.show_channel_menu = nullptr; // TODO: macOS channel context menu

    bridge.show_server_menu = nullptr;  // TODO: macOS server context menu

    bridge.open_share_picker = [bself]() { [bself showSharePicker]; };

    bridge.on_authenticated = [bself]() {
        // Open video/audio streams after successful auth
        bself->_core.net_.open_av_streams();
    };

    bridge.stop_screen_share = [bself]() { [bself stopScreenShare]; };

    bridge.request_keyframe = [bself]() {
        bself->_needsKeyframe = true;
    };

    bridge.clear_video_element = [bself]() {
        if (!bself->_doc) return;
        auto* el = dynamic_cast<VideoElement*>(bself->_doc->GetElementById("screen-share"));
        if (el) el->Clear();
    };

    bridge.start_decode_thread = [bself]() {
        bself->_decoder = std::make_unique<VideoDecoderIOS>();
    };

    bridge.stop_decode_thread = [bself]() {
        bself->_decoder.reset();
    };

    // ── Init AppCore ──────────────────────────────────────────────────────
    if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
        NSLog(@"[Parties] AppCore init failed");
        return;
    }

    // ── Wire macOS-specific model callbacks on top of AppCore defaults ────
    [self installMacOSModelCallbacks];

    // ── Wire parsed video frames to macOS VideoToolbox decoder ────────────
    _core.on_decoded_frame_ready = [bself](const ParsedVideoFrame& frame) {
        if (!bself->_decoder) return;

        // Lazy decoder init on first frame
        if (!bself->_decoder->on_decoded) {
            auto codec = frame.codec;
            if (!bself->_decoder->init(codec, frame.width, frame.height)) {
                std::fprintf(stderr, "[Video] Decoder init failed (codec=%u, %ux%u)\n",
                             static_cast<uint8_t>(codec), frame.width, frame.height);
                bself->_decoder.reset();
                return;
            }
            bself->_decoder->on_decoded = [bself](CVPixelBufferRef buf) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [bself onVideoDecoded:buf];
                    CFRelease(buf);
                });
            };
        }

        bself->_decoder->decode(frame.payload, frame.payload_len, frame.keyframe);
    };

    // ── Load identity ─────────────────────────────────────────────────────
    std::string hostname = NSProcessInfo.processInfo.hostName.UTF8String;
    _core.load_or_generate_identity(hostname);

    // ── Load saved audio prefs ────────────────────────────────────────────
    _core.load_saved_prefs();

    // ── Populate server list ───────────────────────────────────────────────
    _core.refresh_server_list();

    // ── UI document ───────────────────────────────────────────────────────
    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->Show();
        // Mark document as macOS platform so RCSS hides Win32 controls and
        // centres the branding, leaving space for native traffic-light buttons.
        _doc->SetClass("platform-macos", true);
    }

    // ── Mouse tracking area ───────────────────────────────────────────────
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:_metalView.bounds
             options:NSTrackingMouseMoved
                   | NSTrackingActiveInKeyWindow
                   | NSTrackingInVisibleRect
               owner:_metalView
            userInfo:nil];
    [_metalView addTrackingArea:area];
}

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    Backend::SetViewport((int)size.width, (int)size.height);
    if (_rmlContext) {
        _rmlContext->SetDimensions(Rml::Vector2i((int)size.width, (int)size.height));
        float scale = (float)view.window.backingScaleFactor;
        if (scale >= 1.0f)
            _rmlContext->SetDensityIndependentPixelRatio(scale);
    }
}

- (void)drawInMTKView:(MTKView*)view
{
    // Tick shared logic (network messages, FPS counter, audio levels, etc.)
    _core.tick();

    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (!pass) return;

    id<MTLCommandBuffer> buffer = [_commandQueue commandBuffer];

    Backend::BeginFrame(buffer, pass);
    _rmlContext->Update();
    _rmlContext->Render();
    Backend::EndFrame();

    [buffer presentDrawable:view.currentDrawable];
    [buffer commit];
}

// ── macOS-specific model callback overrides ───────────────────────────────────

- (void)installMacOSModelCallbacks
{
    PartiesViewController* bself = self;

    // macOS-specific: start native share button in picker overlay
    _core.model_.on_start_native_share = [bself]() {
        [bself startNativeShare];
    };

    // on_select_share_target is Windows-only (DXGI target list); not used on macOS
    _core.model_.on_select_share_target = nullptr;

    // on_cancel_share: also set use_native_picker for next open
    _core.model_.on_cancel_share = [bself]() {
        bself->_core.model_.show_share_picker = false;
        bself->_core.model_.dirty("show_share_picker");
    };
}

- (void)onVideoDecoded:(CVPixelBufferRef)buf
{
    _core.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

    if (!_doc) return;
    auto* el = dynamic_cast<VideoElement*>(_doc->GetElementById("screen-share"));
    if (!el) return;

    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(buf);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(buf);

    const uint8_t* y_plane  = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
    const uint8_t* uv_plane = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 1);
    uint32_t y_stride  = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
    uint32_t uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 1);

    el->UpdateNV12Frame(y_plane, y_stride, uv_plane, uv_stride, w, h);

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
}

// ── Screen share — sender (macOS SCK) ────────────────────────────────────────

- (void)showSharePicker
{
    _core.model_.use_native_picker  = true;
    _core.model_.show_share_picker  = true;
    _core.model_.dirty("use_native_picker");
    _core.model_.dirty("show_share_picker");
}

- (void)startNativeShare
{
    _core.model_.show_share_picker = false;
    _core.model_.dirty("show_share_picker");

    _capturer = std::make_unique<ScreenCaptureMac>();
    PartiesViewController* bself = self;

    static const uint32_t fps_table[] = { 15, 30, 60, 120 };
    uint32_t capture_fps = fps_table[std::min(_core.model_.share_fps, 3)];

    _capturer->pick_and_start(capture_fps, [bself](bool success) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!success) { bself->_capturer.reset(); return; }
            [bself onCaptureStarted];
        });
    });
}

- (void)onCaptureStarted
{
    _encoder      = std::make_unique<VideoEncoderMac>();
    _encoderReady = false;

    PartiesViewController* bself = self;
    _capturer->on_frame = [bself](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        if (!bself->_encoder) return;

        if (!bself->_encoderReady) {
            uint32_t bitrate = (uint32_t)(bself->_core.model_.share_bitrate * 1000000.0f);
            static const uint32_t fps_table[] = { 15, 30, 60, 120 };
            uint32_t fps = fps_table[std::min(bself->_core.model_.share_fps, 3)];
            if (!bself->_encoder->init(MacVideoCodec::H265, w, h, bitrate, fps)) {
                bself->_encoder.reset(); return;
            }
            bself->_encoderReady = true;

            bself->_encoder->on_encoded = [bself](const uint8_t* data, size_t len, bool is_kf) {
                if (!bself->_core.sharing_) return;
                uint16_t fw = (uint16_t)bself->_capturer->width();
                uint16_t fh = (uint16_t)bself->_capturer->height();
                bself->_core.send_video_frame(data, len, is_kf, fw, fh, VideoCodecId::H265);
            };

            dispatch_async(dispatch_get_main_queue(), ^{
                bself->_core.notify_share_started(VideoCodecId::H265, w, h);
            });
        }

        bool forceKF = bself->_needsKeyframe;
        bself->_needsKeyframe = false;
        bself->_encoder->encode(buf, forceKF);
    };

    _capturer->on_closed = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{ [bself stopScreenShare]; });
    };
}

- (void)stopScreenShare
{
    if (_capturer) _capturer->stop();
    _encoder.reset();
    _capturer.reset();
    _encoderReady  = false;
    _needsKeyframe = false;

    _core.notify_share_stopped();
}

// ── Shutdown ──────────────────────────────────────────────────────────────────

- (void)shutdown
{
    _core.shutdown();
    _soundPlayer.shutdown();
}

@end

// ── PartiesAppDelegate ────────────────────────────────────────────────────────

@implementation PartiesAppDelegate {
    NSWindow*              _window;
    PartiesViewController* _viewController;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    if (!parties::quic_init()) {
        NSLog(@"[Parties] Failed to initialize MsQuic");
    }

    _viewController = [[PartiesViewController alloc] init];

    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled
      | NSWindowStyleMaskClosable
      | NSWindowStyleMaskMiniaturizable
      | NSWindowStyleMaskResizable;

    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    // Hide the native title bar while keeping traffic-light buttons.
    // The content view extends into the title bar area; our RmlUI titlebar
    // renders the background and the native buttons float on top of it.
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility            = NSWindowTitleHidden;
    _window.styleMask                 |= NSWindowStyleMaskFullSizeContentView;
    _window.contentViewController      = _viewController;
    _window.minSize                    = NSMakeSize(800, 500);

    [_window center];
    [_window makeKeyAndOrderFront:nil];

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
    [_viewController shutdown];
    Rml::Shutdown();
    Backend::Shutdown();
    parties::quic_cleanup();
}

@end
