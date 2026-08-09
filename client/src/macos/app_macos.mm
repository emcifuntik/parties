// macOS application — entry point, window, render loop, and app logic.
//
// Mirrors the role of PartiesViewController.mm on iOS, using AppKit instead
// of UIKit.  NSWindow + MTKView host the Metal-backed RmlUI context.

#import "PartiesAppDelegate.h"
#import "context_menu_macos.h"
#import "screen_capture_macos.h"
#import <encdec/apple/video_encoder_macos.h>

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>

// RmlUi Metal backend
#import "../metal/RmlUi_Backend_macOS_Metal.h"
#import "../metal/RmlUi_Renderer_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/PropertyDefinition.h>
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
#include <client/gradient_circle_element.h>
#include <client/custom_elements.h>
#include <client/ui_fixture.h>

#ifdef SENTRY_COCOA_ENABLED
#import <Sentry/Sentry.h>
#import <Sentry/Sentry-Swift.h>
#endif

#include <encdec/apple/VideoDecoderIOS.h>

#ifdef SPARKLE_ENABLED
// Defined in auto_updater_macos.mm
extern void macos_updater_init();
extern void macos_updater_check_now();
extern void macos_updater_check_in_background();
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdio>

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

static std::string macos_ui_fixture_argument()
{
    NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 0; i + 1 < arguments.count; ++i) {
        if ([arguments[i] isEqualToString:@"--ui-fixture"])
            return std::string(arguments[i + 1].UTF8String ?: "");
    }
    return {};
}

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
@property (nonatomic, assign) NSPoint contextMenuPoint;
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
    if (event.modifierFlags & NSEventModifierFlagControl) {
        [self forwardSecondaryClick:event];
        return;
    }
    auto p = [self rmlPoint:event];
    _rmlContext->ProcessMouseMove((int)p.x, (int)p.y, 0);
    _rmlContext->ProcessMouseButtonDown(0, macos_modifiers_to_rml(event.modifierFlags));
}
- (void)mouseUp:(NSEvent*)event
{
    if (!_rmlContext) return;
    // A Control-click is completed as a secondary click in mouseDown:. Sending
    // an unmatched primary up is harmless and guarantees that RmlUi cannot
    // retain a stale primary-button state if AppKit consumes the physical up.
    _rmlContext->ProcessMouseButtonUp(0, macos_modifiers_to_rml(event.modifierFlags));
}
- (void)forwardSecondaryClick:(NSEvent*)event
{
    if (!_rmlContext) return;
    self.contextMenuPoint = [self convertPoint:event.locationInWindow fromView:nil];
    auto p = [self rmlPoint:event];
    int modifiers = macos_modifiers_to_rml(event.modifierFlags);
    _rmlContext->ProcessMouseMove((int)p.x, (int)p.y, modifiers);
    _rmlContext->ProcessMouseButtonDown(1, modifiers);
    // Native menu tracking can consume the physical mouse-up event. Complete
    // the RmlUi click now so its secondary button never remains stuck down.
    _rmlContext->ProcessMouseButtonUp(1, modifiers);
}
- (void)rightMouseDown:(NSEvent*)event
{
    [self forwardSecondaryClick:event];
}
- (void)rightMouseUp:(NSEvent*)event
{
    // The complete secondary click is forwarded from rightMouseDown:.
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
- (void)showPreviewNativeUI;
@end

@implementation PartiesViewController {
    PartiesView*          _metalView;
    id<MTLCommandQueue>   _commandQueue;
    Rml::Context*         _rmlContext;
    Rml::ElementDocument* _doc;
    bool                  _backendInitialized;
    bool                  _rmlInitialized;
    bool                  _debuggerInitialized;
    bool                  _coreInitialized;
    bool                  _soundInitialized;
    bool                  _previewMode;
    std::string           _previewScenario;
    std::unique_ptr<MacOSContextMenuController> _contextMenus;

    // Embedded file interface (must outlive RmlUi)
    EmbeddedFileInterface _fileInterface;

    // Custom element instancers
    parties::rml::ElementRegistry _elementRegistry;

    // AppCore — all shared connection/audio/model logic
    AppCore _core;

    // Sound effects
    SoundPlayer _soundPlayer;

    // Screen share — sender (macOS-specific)
    std::unique_ptr<ScreenCaptureMac> _capturer;
    std::unique_ptr<ScreenCaptureMac> _audioCapturer;
    std::unique_ptr<VideoEncoderMac>  _encoder;
    bool                              _sharing;
    bool                              _encoderReady;
    bool                              _needsKeyframe;
    bool                              _nativePickerActive;
    uint32_t                          _encodeWidth;
    uint32_t                          _encodeHeight;

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
    [_metalView release];
    [device release];
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    _previewScenario = macos_ui_fixture_argument();
    _previewMode = !_previewScenario.empty() && IsUIFixtureScenario(_previewScenario);
    if (!_previewScenario.empty() && !_previewMode)
        NSLog(@"[Parties] Unknown macOS UI fixture: %s", _previewScenario.c_str());

    id<MTLDevice> device = _metalView.device;

    // ── Metal + RmlUi ─────────────────────────────────────────────────────
    _commandQueue = [device newCommandQueue];
    if (!_commandQueue || !Backend::Initialize(device, _metalView)) {
        NSLog(@"[Parties] Failed to initialize the Metal renderer");
        _metalView.paused = YES;
        return;
    }
    _backendInitialized = true;

    Rml::SetFileInterface(&_fileInterface);
    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());

    if (!Rml::Initialise()) {
        NSLog(@"[Parties] Failed to initialize RmlUi");
        Backend::Shutdown();
        _backendInitialized = false;
        _metalView.paused = YES;
        return;
    }
    _rmlInitialized = true;

    // Register window-action as no-op to suppress warnings (used on Windows for caption hit-testing)
    Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
        .AddParser("keyword", "none, caption, close, minimize, maximize");

    parties::client::register_custom_elements(_elementRegistry);

    CGSize physical = _metalView.drawableSize;
    float  dpRatio  = (float)[[NSScreen mainScreen] backingScaleFactor];

    Backend::SetViewport((int)physical.width, (int)physical.height);

    _rmlContext = Rml::CreateContext("main",
        Rml::Vector2i((int)physical.width, (int)physical.height));
    if (!_rmlContext) {
        NSLog(@"[Parties] Failed to create the RmlUi context");
        _metalView.paused = YES;
        return;
    }
    _rmlContext->SetDensityIndependentPixelRatio(dpRatio);
    _metalView.rmlContext = _rmlContext;
    _contextMenus = std::make_unique<MacOSContextMenuController>(_metalView);

#ifndef PARTIES_RETAIL
    _debuggerInitialized = Rml::Debugger::Initialise(_rmlContext);
    if (!_debuggerInitialized)
        NSLog(@"[Parties] Failed to initialize the RmlUi debugger");
#endif

    // ── Fonts ─────────────────────────────────────────────────────────────
    Rml::LoadFontFace("ui/fonts/Inter-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Medium.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Bold.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf", true);
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

    // ── App state ─────────────────────────────────────────────────────────
    _sharing         = false;
    _encoderReady    = false;
    _streamRevealed  = false;
    _needsKeyframe   = false;
    _encodeWidth     = 0;
    _encodeHeight    = 0;

    if (!_previewMode) {
        _soundPlayer.init();
        _soundInitialized = true;
    }

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

    bridge.show_user_menu = [bself](const UserContextWindowRequest& request) {
        if (!bself->_contextMenus) return;
        bself->_contextMenus->SetAnchorPoint(bself->_metalView.contextMenuPoint);

        MacOSUserMenuCallbacks callbacks;
        const int user_id = request.user_id;
        callbacks.set_volume = [bself, user_id](float volume) {
            if (bself->_core.model_.on_user_volume_changed)
                bself->_core.model_.on_user_volume_changed(user_id, volume);
        };
        callbacks.set_music_volume = [bself, user_id](float volume) {
            if (bself->_core.model_.on_user_music_volume_changed)
                bself->_core.model_.on_user_music_volume_changed(user_id, volume);
        };
        callbacks.set_compression = [bself, user_id](bool enabled, float target) {
            if (bself->_core.model_.on_user_compress_changed)
                bself->_core.model_.on_user_compress_changed(user_id, enabled, target);
        };
        callbacks.set_role = [bself, user_id](int role) {
            if (bself->_core.model_.on_set_user_role)
                bself->_core.model_.on_set_user_role(user_id, role);
        };
        callbacks.kick = [bself, user_id] {
            if (bself->_core.model_.on_kick_user)
                bself->_core.model_.on_kick_user(user_id);
        };
        bself->_contextMenus->ShowUser(request, std::move(callbacks));
    };

    bridge.show_channel_menu = [bself](int channel_id, const std::string& name) {
        if (!bself->_contextMenus) return;
        bself->_contextMenus->SetAnchorPoint(bself->_metalView.contextMenuPoint);
        std::vector<MacOSContextAction> actions = {
            {1, "Rename Channel", "Change the channel name"},
            {0, "", "", false, true, true},
            {2, "Delete Channel", "Remove it for everyone", true},
        };
        bself->_contextMenus->ShowActions(name, std::move(actions),
            [bself, channel_id, name](int command) {
                if (command == 1) {
                    bself->_core.model_.rename_channel_id = channel_id;
                    bself->_core.model_.rename_channel_name = name;
                    bself->_core.model_.new_rename_channel_name = name;
                    bself->_core.model_.show_rename_channel = true;
                } else if (command == 2 && bself->_core.model_.on_delete_channel) {
                    bself->_core.model_.on_delete_channel(channel_id);
                }
            });
    };

    bridge.show_server_menu = [bself](int server_id) {
        if (!bself->_contextMenus) return;
        bself->_contextMenus->SetAnchorPoint(bself->_metalView.contextMenuPoint);
        std::vector<MacOSContextAction> actions = {
            {1, "Change Server Nickname", "Override your name for this party"},
            {0, "", "", false, true, true},
            {2, "Remove Saved Party", "Delete this saved connection", true},
        };
        bself->_contextMenus->ShowActions("Saved Party", std::move(actions),
            [bself, server_id](int command) {
                if (command == 1 && bself->_core.server_model_.on_edit_server_nickname)
                    bself->_core.server_model_.on_edit_server_nickname(server_id);
                else if (command == 2 && bself->_core.server_model_.on_delete_server)
                    bself->_core.server_model_.on_delete_server(server_id);
            });
    };

    bridge.show_message_menu = [bself](int64_t message_id) {
        if (!bself->_contextMenus) return;

        std::string text;
        bool pinned = false;
        for (const auto& message : bself->_core.chat_model_.messages.get()) {
            if (message.id == message_id) {
                text = std::string(message.text);
                pinned = message.pinned;
                break;
            }
        }

        bself->_contextMenus->SetAnchorPoint(bself->_metalView.contextMenuPoint);
        std::vector<MacOSContextAction> actions;
        if (!text.empty())
            actions.push_back({1, "Copy Text", "Copy the message to the clipboard"});
        actions.push_back({2, pinned ? "Unpin Message" : "Pin Message",
                           pinned ? "Remove it from pinned messages" : "Keep it visible"});
        actions.push_back({0, "", "", false, true, true});
        actions.push_back({3, "Delete Message", "Remove it permanently", true});

        bself->_contextMenus->ShowActions("Message", std::move(actions),
            [bself, message_id, text = std::move(text), pinned](int command) {
                if (command == 1) {
                    NSString* value = [NSString stringWithUTF8String:text.c_str()];
                    if (value) {
                        [[NSPasteboard generalPasteboard] clearContents];
                        [[NSPasteboard generalPasteboard] setString:value
                                                            forType:NSPasteboardTypeString];
                    }
                } else if (command == 2) {
                    auto& callback = pinned ? bself->_core.chat_model_.on_unpin_message
                                            : bself->_core.chat_model_.on_pin_message;
                    if (callback) callback(message_id);
                } else if (command == 3 && bself->_core.chat_model_.on_delete_message) {
                    bself->_core.chat_model_.on_delete_message(message_id);
                }
            });
    };

    // macOS goes straight to the system picker. The intermediate RML picker is
    // useful on Windows, but adds an unnecessary second click on Apple platforms.
    bridge.open_share_picker = [bself]() { [bself startNativeShare]; };
    bridge.open_audio_share_picker = [bself]() { [bself startNativeAudioShare]; };

    bridge.on_authenticated = [bself]() {
        // Open video/audio streams after successful auth
        bself->_core.net_.open_av_streams();
    };

    bridge.stop_screen_share = [bself]() { [bself stopScreenShare]; };
    bridge.stop_audio_share = [bself]() { [bself stopAudioShare]; };

    bridge.request_keyframe = [bself]() {
        bself->_needsKeyframe = true;
    };

    // The viewer is now a data-for grid of per-sharer cells ("screen-share-<id>");
    // a stream's cell is destroyed by the binding when it drops out of
    // model_.watched, so there is no single element to clear here.
    bridge.clear_video_element = []() {};

    if (_previewMode) {
        // Deterministic visual harness: production AppKit shell, Metal backend,
        // RML document and data models, with no audio, database or network I/O.
        if (!_core.server_model_.init(_rmlContext) ||
            !_core.model_.init(_rmlContext) ||
            !_core.chat_model_.init(_rmlContext)) {
            NSLog(@"[Parties] Failed to initialize macOS UI fixture models");
            return;
        }
        PopulateUIFixture(_core, _previewScenario, true);
        NSLog(@"[Parties] Loaded macOS UI fixture: %s", _previewScenario.c_str());
    } else {
        // ── Init AppCore ──────────────────────────────────────────────────
        if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
            NSLog(@"[Parties] AppCore init failed");
            return;
        }
        _coreInitialized = true;

        // ── Wire macOS-specific model callbacks on top of AppCore defaults ─
        [self installMacOSModelCallbacks];

        // ── Wire video frame reception to local macOS decoder ─────────────
        _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
            [bself onVideoFrameData:sender_id data:data len:len];
        };

        // ── Load identity and saved state ─────────────────────────────────
        std::string hostname = NSProcessInfo.processInfo.hostName.UTF8String;
        _core.load_or_generate_identity(hostname);
        _core.load_saved_prefs();
        _core.refresh_server_list();
    }

    // ── UI document ───────────────────────────────────────────────────────
    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        // Mark document as macOS platform so RCSS hides Win32 controls and
        // centres the branding, leaving space for native traffic-light buttons.
        _doc->SetClass("platform-macos", true);
        _doc->SetClass("platform-desktop", true);
        _doc->Show();
        if (_previewMode) {
            ApplyUIFixtureDocument(_doc, _previewScenario);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(350 * NSEC_PER_MSEC)),
                           dispatch_get_main_queue(), ^{ [self showPreviewNativeUI]; });
        }
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

- (void)showPreviewNativeUI
{
    if (!_previewMode || !_contextMenus) return;

    _contextMenus->SetAnchorPoint(NSMakePoint(420.0, 390.0));
    if (_previewScenario == "native-user") {
        UserContextWindowRequest request;
        request.user_id = 2;
        request.name = "IceTroll";
        request.channel_name = "General";
        request.role = 1;
        request.can_manage_roles = true;
        request.can_kick = true;
        request.volume = 0.86f;
        request.music_volume = 0.64f;
        request.compression = true;
        request.compression_target = 0.55f;
        _contextMenus->ShowUser(request, {});
    } else if (_previewScenario == "native-channel") {
        _contextMenus->ShowActions("General", {
            {1, "Rename Channel", "Change the channel name"},
            {0, "", "", false, true, true},
            {2, "Delete Channel", "Remove it for everyone", true},
        }, [](int) {});
    } else if (_previewScenario == "native-server") {
        _contextMenus->ShowActions("Night Shift", {
            {1, "Change Server Nickname", "Override your name for this party"},
            {0, "", "", false, true, true},
            {2, "Remove Saved Party", "Delete this saved connection", true},
        }, [](int) {});
    } else if (_previewScenario == "native-message") {
        _contextMenus->ShowActions("Message", {
            {1, "Copy Text", "Copy the message to the clipboard"},
            {2, "Pin Message", "Keep it visible"},
            {0, "", "", false, true, true},
            {3, "Delete Message", "Remove it permanently", true},
        }, [](int) {});
    } else if (_previewScenario == "native-share-picker") {
        [self startNativeShare];
    } else if (_previewScenario == "native-audio-picker") {
        [self startNativeAudioShare];
    }
}

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    if (!_backendInitialized) return;
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
    if (!_backendInitialized || !_rmlContext || !_commandQueue) return;

    // Tick shared logic (network messages, FPS counter, audio levels, etc.)
    if (_coreInitialized)
        _core.tick();

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
                    elem->SetInnerRML(Rml::String(std::to_string(_core.model_.ping_ms.get()) + " ms"));
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
            [bself startNativeShare];
    };
    _core.model_.on_toggle_audio_share = [bself]() {
        if (bself->_core.model_.is_audio_sharing)
            [bself stopAudioShare];
        else
            [bself startNativeAudioShare];
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
    // Single hardware decoder: a chip toggle just switches to that single stream.
    _core.model_.on_toggle_watch = [bself](int id) {
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
    // The viewer is a grid of per-sharer cells; route to this sharer's cell.
    // macOS is single-select, so viewing_sharer_ is the one being watched.
    std::string elem_id = "screen-share-" + std::to_string(_core.viewing_sharer_.load());
    auto* el = dynamic_cast<VideoElement*>(_doc->GetElementById(elem_id));
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

- (void)startNativeShare
{
    NSLog(@"[ScreenShare] startNativeShare");
    if (_nativePickerActive || _capturer || _sharing) return;
    if (!_previewMode && (!_core.authenticated_ || _core.current_channel_ == 0)) return;
    if (_core.model_.router.is(DocumentRoute::SharePicker))
        _core.model_.router.back();

    _capturer = std::make_unique<ScreenCaptureMac>();
    _nativePickerActive = true;
    PartiesViewController* bself = self;

    static const uint32_t fps_table[] = { 15, 30, 60, 120 };
    uint32_t capture_fps = fps_table[std::min(_core.model_.share_fps.get(), 3)];

    // User scale (0=Source, 1=x0.75, 2=x0.5, 3=x0.25). ScreenCaptureKit applies
    // this (and a long-edge cap) on the GPU, so frames arrive at encode size.
    static const float scale_factors[] = {1.0f, 0.75f, 0.5f, 0.25f};
    float output_scale = scale_factors[std::max(0, std::min(_core.model_.share_scale.get(), 3))];

    _capturer->pick_and_start(capture_fps, output_scale, false, [bself](bool success) {
        NSLog(@"[ScreenShare] pick_and_start callback: success=%d", success);
        dispatch_async(dispatch_get_main_queue(), ^{
            bself->_nativePickerActive = false;
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
            uint32_t fps = fps_table[std::min(bself->_core.model_.share_fps.get(), 3)];

            // Codec selection: 0=AV1, 1=H265, 2=H264
            MacVideoCodec mac_codec = (bself->_core.model_.share_codec == 0) ? MacVideoCodec::AV1
                                    : (bself->_core.model_.share_codec == 2) ? MacVideoCodec::H264
                                                                             : MacVideoCodec::H265;

            // Frames already arrive at the final encode size: ScreenCaptureKit
            // applies the user scale + long-edge cap on the GPU (see
            // pick_and_start). Encode the buffer as-is — no CPU downscale. Even
            // dimensions are guaranteed by the capture config.
            uint32_t enc_w = w & ~1u;
            uint32_t enc_h = h & ~1u;
            if (enc_w < 64) enc_w = 64;
            if (enc_h < 64) enc_h = 64;
            bself->_encodeWidth  = enc_w;
            bself->_encodeHeight = enc_h;

            if (!bself->_encoder->init(mac_codec, enc_w, enc_h, bitrate, fps)) {
                bself->_encoder.reset(); return;
            }
            bself->_encoderReady = true;

            // Use the codec the encoder actually initialized with, not the
            // requested one: AV1 falls back to H265 on Apple Silicon (no AV1
            // encoder). Tagging the wire with the real codec keeps receivers
            // from decoding HEVC frames as AV1.
            MacVideoCodec eff_codec = bself->_encoder->actual_codec();
            VideoCodecId wire_codec = (eff_codec == MacVideoCodec::AV1)  ? VideoCodecId::AV1
                                   : (eff_codec == MacVideoCodec::H264) ? VideoCodecId::H264
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

        // Frame is already at encode resolution (scaled on the GPU by
        // ScreenCaptureKit). Encode it directly — no per-frame Core Image pass.
        bself->_encoder->encode(buf, forceKF);
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

    if (_core.authenticated_)
        _core.net_.send_message(ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

- (void)startNativeAudioShare
{
    NSLog(@"[AudioShare] startNativeAudioShare");
    if (_nativePickerActive || _audioCapturer || _core.model_.is_audio_sharing) return;
    if (!_previewMode && (!_core.authenticated_ || _core.current_channel_ == 0)) return;
    if (_core.model_.router.is(DocumentRoute::SharePicker))
        _core.model_.router.back();

    _audioCapturer = std::make_unique<ScreenCaptureMac>();
    _nativePickerActive = true;
    PartiesViewController* bself = self;

    // Audio-only SCK capture requests mono 48 kHz PCM, matching AudioEngine's
    // secondary VOICE2 encoder without a per-callback allocation or downmix.
    _audioCapturer->on_audio = [bself](const float* samples, uint32_t frame_count) {
        if (!bself->_coreInitialized || !bself->_core.authenticated_ ||
            !bself->_core.model_.is_audio_sharing)
            return;
        bself->_core.audio_.push_secondary_pcm(samples, static_cast<int>(frame_count));
    };
    _audioCapturer->on_closed = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{ [bself stopAudioShare]; });
    };

    _audioCapturer->pick_and_start(30, 1.0f, true, [bself](bool success) {
        NSLog(@"[AudioShare] pick_and_start callback: success=%d", success);
        dispatch_async(dispatch_get_main_queue(), ^{
            bself->_nativePickerActive = false;
            if (!success) {
                bself->_audioCapturer.reset();
                return;
            }
            bself->_core.model_.is_audio_sharing = true;
            bself->_core.model_.audio_share_target_name = "Application audio";
        });
    });
}

- (void)stopAudioShare
{
    if (_audioCapturer) {
        // Prevent SCStream's close callback from recursively re-entering this
        // method while the owner is being reset.
        _audioCapturer->on_closed = {};
        _audioCapturer->stop();
        _audioCapturer.reset();
    }
    _core.model_.is_audio_sharing = false;
    _core.model_.audio_share_target_name = "";
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

    // Populate the viewer grid model with this single stream (single hardware
    // decoder → exactly one cell, id "screen-share-<id>").
    _core.set_single_watched(sharerId);
    _streamRevealed = false;
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

    _core.set_single_watched(0);
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
    if (_contextMenus) {
        _contextMenus->Close();
        _contextMenus.reset();
    }
    _nativePickerActive = false;
    if (_capturer) {
        _capturer->on_closed = {};
        _capturer->stop();
        _capturer.reset();
    }
    if (_audioCapturer) {
        _audioCapturer->on_closed = {};
        _audioCapturer->stop();
        _audioCapturer.reset();
    }
    if (_coreInitialized) {
        _core.shutdown();
        _coreInitialized = false;
    }
    if (_soundInitialized) {
        _soundPlayer.shutdown();
        _soundInitialized = false;
    }

#ifndef PARTIES_RETAIL
    if (_debuggerInitialized) {
        Rml::Debugger::Shutdown();
        _debuggerInitialized = false;
    }
#endif
    if (_rmlContext) {
        Rml::RemoveContext(_rmlContext->GetName());
        _rmlContext = nullptr;
        _metalView.rmlContext = nullptr;
    }
    if (_rmlInitialized) {
        Rml::Shutdown();
        _rmlInitialized = false;
    }
    if (_backendInitialized) {
        Backend::Shutdown();
        _backendInitialized = false;
    }
    [_commandQueue release];
    _commandQueue = nil;
}

@end

// ── PartiesAppDelegate ────────────────────────────────────────────────────────

@implementation PartiesAppDelegate {
    NSWindow*              _window;
    PartiesViewController* _viewController;
    bool                   _quicInitialized;
    bool                   _previewMode;
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

    const std::string fixture = macos_ui_fixture_argument();
    _previewMode = !fixture.empty() && IsUIFixtureScenario(fixture);
    if (!_previewMode) {
        if (!parties::quic_init()) {
            NSLog(@"[Parties] Failed to initialize MsQuic");
        } else {
            _quicInitialized = true;
        }
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

    [self installMainMenu];

#ifdef SPARKLE_ENABLED
    if (!_previewMode) {
        macos_updater_init();
        // Proactively check shortly after launch so an available update is offered
        // promptly, rather than only on Sparkle's periodic background schedule.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(4 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            macos_updater_check_in_background();
        });
    }
#endif
}

// Build a minimal standard application menu. The app otherwise ships no menu
// bar; this provides About / Hide / Quit and, crucially, a user-triggerable
// "Check for Updates…" item wired to Sparkle.
- (void)installMainMenu
{
    NSString* appName = [[NSProcessInfo processInfo] processName];

    NSMenu* mainMenu = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] init];
    appMenuItem.submenu = appMenu;

    [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %@", appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

#ifdef SPARKLE_ENABLED
    NSMenuItem* updateItem =
        [appMenu addItemWithTitle:@"Check for Updates…"
                           action:@selector(checkForUpdatesAction:)
                    keyEquivalent:@""];
    updateItem.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
#endif

    NSMenuItem* hideItem =
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %@", appName]
                           action:@selector(hide:)
                    keyEquivalent:@"h"];
    hideItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;

    NSMenuItem* hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;

    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem =
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
    quitItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;

    NSApp.mainMenu = mainMenu;
}

#ifdef SPARKLE_ENABLED
- (void)checkForUpdatesAction:(id)sender
{
    macos_updater_check_now();
}
#endif

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
    [_viewController shutdown];
    if (_quicInitialized) {
        parties::quic_cleanup();
        _quicInitialized = false;
    }
}

@end
