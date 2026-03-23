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
#import <CoreImage/CoreImage.h>

// RmlUi Metal backend
#import "../metal/RmlUi_Backend_macOS_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#ifndef PARTIES_RETAIL
#include <RmlUi/Debugger.h>
#endif

// Parties protocol
#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/serialization.h>
#include <parties/audio_common.h>
#include <parties/codec.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/video_common.h>

// Shared client code
#include <parties/quic_common.h>
#include <client/app_core.h>
#include <client/sound_player.h>
#include <client/rmlui_backend.h>
#include <client/video_element.h>
#include <client/level_meter_element.h>
#include <client/gradient_circle_element.h>

#ifdef SENTRY_COCOA_ENABLED
#import <Sentry/Sentry.h>
#import <Sentry/Sentry-Swift.h>
#endif

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

    // Custom element instancers
    VideoElementInstancer _videoInstancer;
    LevelMeterInstancer   _levelMeterInstancer;
    GradientCircleInstancer _gradientCircleInstancer;
    LevelMeterElement*    _levelMeter;

    // AppCore — all shared connection/audio/model logic
    AppCore _core;

    // Sound effects
    SoundPlayer _soundPlayer;

    // Screen share — sender (macOS-specific)
    std::unique_ptr<ScreenCaptureMac> _capturer;
    std::unique_ptr<VideoEncoderMac>  _encoder;
    bool                              _sharing;
    bool                              _encoderReady;
    bool                              _needsKeyframe;
    uint32_t                          _encodeWidth;
    uint32_t                          _encodeHeight;
    bool                              _needsScale;
    CIContext*                        _ciContext;

    // Stream audio — Opus encoder for screen share audio
    parties::OpusCodec                _streamAudioEncoder;
    bool                              _streamAudioReady;
    std::vector<float>                _audioBuf;    // accumulation buffer
    size_t                            _audioPos;
    uint8_t                           _opusBuf[parties::audio::MAX_OPUS_PACKET];

    // Screen share — receiver (macOS-specific VideoToolbox decoder)
    std::unique_ptr<VideoDecoderIOS>  _decoder;
    bool                              _streamRevealed;

    // FPS counter
    uint32_t _fpsFrameCount;
    std::chrono::steady_clock::time_point _fpsLastUpdate;
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
    Rml::Factory::RegisterElementInstancer("level_meter", &_levelMeterInstancer);
    Rml::Factory::RegisterElementInstancer("gradient_circle", &_gradientCircleInstancer);

    CGSize physical = _metalView.drawableSize;
    float  dpRatio  = (float)[[NSScreen mainScreen] backingScaleFactor];

    Backend::SetViewport((int)physical.width, (int)physical.height);

    _rmlContext = Rml::CreateContext("main",
        Rml::Vector2i((int)physical.width, (int)physical.height));
    _rmlContext->SetDensityIndependentPixelRatio(dpRatio);
    _metalView.rmlContext = _rmlContext;

#ifndef PARTIES_RETAIL
    Rml::Debugger::Initialise(_rmlContext);
#endif

    // ── Fonts ─────────────────────────────────────────────────────────────
    Rml::LoadFontFace("ui/fonts/Inter-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Medium.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Bold.ttf");

    // ── App state ─────────────────────────────────────────────────────────
    _sharing         = false;
    _encoderReady    = false;
    _streamRevealed  = false;
    _needsKeyframe   = false;
    _encodeWidth     = 0;
    _encodeHeight    = 0;
    _needsScale      = false;
    _ciContext       = nil;

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
    bridge.set_notification_volume = [bself](float v) {
        bself->_soundPlayer.set_volume(v);
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

    // ── Init AppCore ──────────────────────────────────────────────────────
    if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
        NSLog(@"[Parties] AppCore init failed");
        return;
    }

    // ── Wire macOS-specific model callbacks on top of AppCore defaults ────
    [self installMacOSModelCallbacks];

    // ── Wire video frame reception to local macOS decoder ─────────────────
    _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
        [bself onVideoFrameData:sender_id data:data len:len];
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
        _levelMeter = static_cast<LevelMeterElement*>(_doc->GetElementById("voice-level-meter"));
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

    // Update voice level meter
    if (_levelMeter && _core.model_.is_connected) {
        _levelMeter->SetLevel(_core.audio_.voice_level());
        _levelMeter->SetThreshold(_core.model_.vad_threshold);
    }

    // Update FPS + ping in titlebar (once per second)
    _fpsFrameCount++;
    auto now_fps = std::chrono::steady_clock::now();
    float elapsed_fps = std::chrono::duration<float>(now_fps - _fpsLastUpdate).count();
    if (elapsed_fps >= 1.0f) {
        int fps = static_cast<int>(_fpsFrameCount / elapsed_fps);
        _fpsFrameCount = 0;
        _fpsLastUpdate = now_fps;
        if (_doc) {
            if (auto* elem = _doc->GetElementById("titlebar-fps"))
                elem->SetInnerRML(Rml::String(std::to_string(fps) + " fps"));
            if (auto* elem = _doc->GetElementById("titlebar-ping")) {
                if (_core.model_.is_connected)
                    elem->SetInnerRML(Rml::String(std::to_string(_core.model_.ping_ms) + " ms"));
                else
                    elem->SetInnerRML("");
            }
        }
    }

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

    // Override on_toggle_share: macOS uses SCK native picker
    _core.model_.on_toggle_share = [bself]() {
        if (bself->_sharing)
            [bself stopScreenShare];
        else
            [bself showSharePicker];
    };

    // macOS-specific: start native share button in picker overlay
    _core.model_.on_start_native_share = [bself]() {
        [bself startNativeShare];
    };

    // on_select_share_target is Windows-only; clear it if AppCore set it
    _core.model_.on_select_share_target = nullptr;

    // Override watch/stop watching to manage the local VideoDecoderIOS
    _core.model_.on_watch_sharer = [bself](int id) {
        [bself watchSharer:static_cast<UserId>(id)];
    };
    _core.model_.on_select_sharer = [bself](int id) {
        [bself watchSharer:static_cast<UserId>(id)];
    };
    _core.model_.on_stop_watching = [bself]() {
        [bself stopWatching];
    };
}

// ── Video frame routing (macOS VideoToolbox decoder) ─────────────────────────

- (void)onVideoFrameData:(uint32_t)sender_id data:(const uint8_t*)data len:(size_t)len
{
    // data = [fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][encoded(N)]
    if (len < 14) return;
    if (sender_id != _core.viewing_sharer_ || !_decoder) return;

    uint8_t  flags = data[8];
    uint16_t w, h;
    std::memcpy(&w, data + 9,  2);
    std::memcpy(&h, data + 11, 2);
    uint8_t codec_id = data[13];

    bool is_keyframe = (flags & VIDEO_FLAG_KEYFRAME) != 0;

    if (_core.awaiting_keyframe_ && !is_keyframe) return;
    _core.awaiting_keyframe_ = false;

    // Lazy decoder init on first frame
    if (!_decoder->on_decoded) {
        auto codec = static_cast<VideoCodecId>(codec_id);
        if (!_decoder->init(codec, w, h)) {
            std::fprintf(stderr, "[Video] Decoder init failed (codec=%u, %ux%u)\n",
                         codec_id, w, h);
            _decoder.reset();
            return;
        }
        PartiesViewController* bself = self;
        _decoder->on_decoded = [bself](CVPixelBufferRef buf) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [bself onVideoDecoded:buf];
                CFRelease(buf);
            });
        };
    }

    _decoder->decode(data + 14, len - 14, is_keyframe);
}

- (void)onVideoDecoded:(CVPixelBufferRef)buf
{
    // Reveal video area on first frame (deferred from watch_sharer)
    if (!_streamRevealed) {
        _streamRevealed = true;
        _core.model_.dirty("viewing_sharer_id");
    }
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
    NSLog(@"[ScreenShare] showSharePicker");
    _core.model_.use_native_picker  = true;
    _core.model_.show_share_picker  = true;
    _core.model_.dirty("use_native_picker");
    _core.model_.dirty("show_share_picker");
}

- (void)startNativeShare
{
    NSLog(@"[ScreenShare] startNativeShare");
    _core.model_.show_share_picker = false;
    _core.model_.dirty("show_share_picker");

    _capturer = std::make_unique<ScreenCaptureMac>();
    PartiesViewController* bself = self;

    static const uint32_t fps_table[] = { 15, 30, 60, 120 };
    uint32_t capture_fps = fps_table[std::min(_core.model_.share_fps, 3)];

    _capturer->pick_and_start(capture_fps, [bself](bool success) {
        NSLog(@"[ScreenShare] pick_and_start callback: success=%d", success);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!success) { NSLog(@"[ScreenShare] capture failed, resetting"); bself->_capturer.reset(); return; }
            [bself onCaptureStarted];
        });
    });
}

- (void)onCaptureStarted
{
    NSLog(@"[ScreenShare] onCaptureStarted");
    _encoder      = std::make_unique<VideoEncoderMac>();
    _encoderReady = false;

    PartiesViewController* bself = self;
    _capturer->on_frame = [bself](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        if (!bself->_encoder) return;

        if (!bself->_encoderReady) {
            uint32_t bitrate = (uint32_t)(bself->_core.model_.share_bitrate * 1000000.0f);
            static const uint32_t fps_table[] = { 15, 30, 60, 120 };
            uint32_t fps = fps_table[std::min(bself->_core.model_.share_fps, 3)];

            // Codec selection: 0=AV1, 1=H265, 2=H264
            MacVideoCodec mac_codec = (bself->_core.model_.share_codec == 0) ? MacVideoCodec::AV1
                                    : (bself->_core.model_.share_codec == 2) ? MacVideoCodec::H264
                                                                             : MacVideoCodec::H265;

            // Apply scale factor
            static const float scale_factors[] = {1.0f, 0.75f, 0.5f, 0.25f};
            int scale_idx = std::max(0, std::min(bself->_core.model_.share_scale, 3));
            float sf = scale_factors[scale_idx];
            uint32_t enc_w = ((uint32_t)(w * sf) + 1) & ~1u;
            uint32_t enc_h = ((uint32_t)(h * sf) + 1) & ~1u;
            if (enc_w < 64) enc_w = 64;
            if (enc_h < 64) enc_h = 64;
            bself->_encodeWidth  = enc_w;
            bself->_encodeHeight = enc_h;
            bself->_needsScale   = (enc_w != w || enc_h != h);

            if (!bself->_encoder->init(mac_codec, enc_w, enc_h, bitrate, fps)) {
                bself->_encoder.reset(); return;
            }
            bself->_encoderReady = true;

            VideoCodecId wire_codec = (mac_codec == MacVideoCodec::AV1)  ? VideoCodecId::AV1
                                   : (mac_codec == MacVideoCodec::H264) ? VideoCodecId::H264
                                                                        : VideoCodecId::H265;
            bself->_encoder->on_encoded = [bself, wire_codec](const uint8_t* data, size_t len, bool is_kf) {
                if (!bself->_core.authenticated_) return;

                uint32_t fn = bself->_core.video_frame_number_++;
                uint32_t ts = 0;
                uint8_t  flags = is_kf ? VIDEO_FLAG_KEYFRAME : 0;
                uint16_t fw = (uint16_t)bself->_encodeWidth;
                uint16_t fh = (uint16_t)bself->_encodeHeight;
                uint8_t  codec = static_cast<uint8_t>(wire_codec);

                std::vector<uint8_t> pkt(1 + 4 + 4 + 1 + 2 + 2 + 1 + len);
                size_t off = 0;
                pkt[off++] = VIDEO_FRAME_PACKET_TYPE;
                std::memcpy(pkt.data() + off, &fn, 4);    off += 4;
                std::memcpy(pkt.data() + off, &ts, 4);    off += 4;
                pkt[off++] = flags;
                std::memcpy(pkt.data() + off, &fw, 2);    off += 2;
                std::memcpy(pkt.data() + off, &fh, 2);    off += 2;
                pkt[off++] = codec;
                std::memcpy(pkt.data() + off, data, len);
                bself->_core.net_.send_video(pkt.data(), pkt.size(), true);
                bself->_core.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

                // Self-preview: feed encoded frame back to local decoder
                if (bself->_core.viewing_sharer_ == bself->_core.user_id_ && bself->_decoder) {
                    // onVideoFrameData expects [fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][data(N)]
                    [bself onVideoFrameData:bself->_core.user_id_
                                       data:pkt.data() + 1
                                        len:pkt.size() - 1];
                }
            };

            dispatch_async(dispatch_get_main_queue(), ^{
                bself->_sharing = true;
                bself->_core.model_.is_sharing = true;
                bself->_core.model_.dirty("is_sharing");
                // Server expects codec(1) + width(2) + height(2); placeholder values
                // are updated by SCREEN_SHARE_UPDATE once the encoder produces real dims.
                uint8_t payload[5] = {};
                bself->_core.net_.send_message(ControlMessageType::SCREEN_SHARE_START, payload, sizeof(payload));

                // Auto-watch self for local preview
                [bself watchSharer:bself->_core.user_id_];
            });
        }

        bool forceKF = bself->_needsKeyframe;
        bself->_needsKeyframe = false;

        if (bself->_needsScale) {
            // GPU-accelerated downscale via Core Image
            CIImage* srcImage = [CIImage imageWithCVPixelBuffer:buf];
            float sx = (float)bself->_encodeWidth  / (float)w;
            float sy = (float)bself->_encodeHeight / (float)h;
            CIImage* scaled = [srcImage imageByApplyingTransform:CGAffineTransformMakeScale(sx, sy)];

            if (!bself->_ciContext)
                bself->_ciContext = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @NO}];

            CVPixelBufferRef scaledBuf = nullptr;
            NSDictionary* attrs = @{
                (id)kCVPixelBufferWidthKey:  @(bself->_encodeWidth),
                (id)kCVPixelBufferHeightKey: @(bself->_encodeHeight),
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}
            };
            CVPixelBufferCreate(kCFAllocatorDefault, bself->_encodeWidth, bself->_encodeHeight,
                                kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &scaledBuf);
            if (scaledBuf) {
                [bself->_ciContext render:scaled toCVPixelBuffer:scaledBuf];
                bself->_encoder->encode(scaledBuf, forceKF);
                CVPixelBufferRelease(scaledBuf);
            }
        } else {
            bself->_encoder->encode(buf, forceKF);
        }
    };

    // Stream audio: init Opus encoder and accumulation buffer (48 kHz, stereo, 20 ms frames)
    static constexpr int kAudioFrameSize = 960;  // 20ms at 48kHz
    static constexpr int kAudioChannels  = 2;
    _streamAudioReady = _streamAudioEncoder.init_encoder(48000, kAudioChannels, 64000);
    _audioBuf.resize(kAudioFrameSize * kAudioChannels, 0.0f);
    _audioPos = 0;

    if (_streamAudioReady) {
        _capturer->on_audio = [bself](const float* samples, uint32_t frame_count) {
            if (!bself->_sharing || !bself->_core.authenticated_ || !bself->_streamAudioReady)
                return;

            const size_t samples_per_opus_frame = kAudioFrameSize * kAudioChannels;
            size_t input_pos = 0;
            size_t total_samples = (size_t)frame_count * kAudioChannels;

            while (input_pos < total_samples) {
                size_t space = samples_per_opus_frame - bself->_audioPos;
                size_t copy  = std::min(space, total_samples - input_pos);
                std::memcpy(bself->_audioBuf.data() + bself->_audioPos,
                            samples + input_pos, copy * sizeof(float));
                bself->_audioPos += copy;
                input_pos += copy;

                if (bself->_audioPos >= samples_per_opus_frame) {
                    int encoded = bself->_streamAudioEncoder.encode(
                        bself->_audioBuf.data(), kAudioFrameSize,
                        bself->_opusBuf, parties::audio::MAX_OPUS_PACKET);
                    if (encoded > 0) {
                        std::vector<uint8_t> pkt(1 + encoded);
                        pkt[0] = STREAM_AUDIO_PACKET_TYPE;
                        std::memcpy(pkt.data() + 1, bself->_opusBuf, encoded);
                        bself->_core.net_.send_data(pkt.data(), pkt.size());
                    }
                    bself->_audioPos = 0;
                }
            }
        };
        NSLog(@"[ScreenShare] Stream audio capture enabled (48kHz stereo Opus)");
    }

    _capturer->on_closed = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{ [bself stopScreenShare]; });
    };
}

- (void)stopScreenShare
{
    if (_capturer) _capturer->stop();
    _encoder.reset();
    _capturer.reset();
    _sharing       = false;
    _encoderReady  = false;
    _needsKeyframe = false;
    _streamAudioReady = false;
    _audioPos = 0;
    _core.video_frame_number_ = 0;

    _core.model_.is_sharing = false;
    _core.model_.dirty("is_sharing");

    if (_core.authenticated_)
        _core.net_.send_message(ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

// ── Screen share — receiver (macOS) ──────────────────────────────────────────

- (void)watchSharer:(UserId)sharerId
{
    _core.viewing_sharer_   = sharerId;
    _core.awaiting_keyframe_ = true;
    _decoder = std::make_unique<VideoDecoderIOS>();
    // on_decoded is wired lazily in onVideoFrameData on first frame

    [self sendPLI:sharerId];

    uint32_t id32 = static_cast<uint32_t>(sharerId);
    _core.net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                             (const uint8_t*)&id32, sizeof(id32));

    _core.model_.viewing_sharer_id = static_cast<int>(sharerId);
    _streamRevealed = false;
    // Don't dirty yet — onVideoDecoded dirties on first frame to avoid black flash
}

- (void)stopWatching
{
    _core.viewing_sharer_   = 0;
    _decoder.reset();
    _core.awaiting_keyframe_ = false;
    _streamRevealed = false;

    uint32_t zero = 0;
    _core.net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                             (const uint8_t*)&zero, sizeof(zero));

    _core.model_.viewing_sharer_id = 0;
    _core.model_.dirty("viewing_sharer_id");

    if (_doc) {
        auto* el = dynamic_cast<VideoElement*>(_doc->GetElementById("screen-share"));
        if (el) el->Clear();
    }
}

- (void)sendPLI:(UserId)targetId
{
    uint32_t id32 = static_cast<uint32_t>(targetId);
    std::vector<uint8_t> pkt(6);
    pkt[0] = VIDEO_CONTROL_TYPE;
    pkt[1] = VIDEO_CTL_PLI;
    std::memcpy(pkt.data() + 2, &id32, 4);
    _core.net_.send_video(pkt.data(), pkt.size(), true);
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
#ifdef SENTRY_COCOA_ENABLED
#ifdef SENTRY_DSN_VALUE
    [SentrySDK startWithConfigureOptions:^(SentryOptions *options) {
        options.dsn = @SENTRY_DSN_VALUE;
#ifdef PARTIES_RETAIL
        options.environment = @"production";
#else
        options.environment = @"development";
#endif
        options.enableCrashHandler = YES;
        options.enableAppHangTracking = YES;
        options.sendDefaultPii = YES;
        options.tracesSampleRate = @1.0;
    }];
#endif
#endif

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
