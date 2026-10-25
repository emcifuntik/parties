#import "PartiesAppDelegate.h"
#import "context_menu_macos.h"
#import "PartiesStatusItemController.h"
#import "pip_host_macos.h"
#import "screen_capture_macos.h"
#import <encdec/apple/video_encoder_macos.h>

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>

#import "../metal/RmlUi_Backend_macOS_Metal.h"
#import "../metal/RmlUi_Renderer_Metal.h"

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

#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/serialization.h>
#include <parties/audio_common.h>
#include <parties/codec.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/video_common.h>

#include <parties/quic_common.h>
#include <client/app_core.h>
#include <client/sound_player.h>
#include <client/rmlui_backend.h>
#include <client/video_element.h>
#include <client/video_frame_router.h>
#include <client/pip_geometry.h>
#include <client/gradient_circle_element.h>
#include <client/custom_elements.h>
#include <client/ui_fixture.h>
#include <client/ui_layout_audit.h>

#ifdef SENTRY_COCOA_ENABLED
#import <Sentry/Sentry.h>
#import <Sentry/Sentry-Swift.h>
#endif

#include <encdec/apple/VideoDecoderIOS.h>

#ifdef SPARKLE_ENABLED
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
    case 0x74: return Rml::Input::KI_PRIOR;
    case 0x79: return Rml::Input::KI_NEXT;
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

@interface PartiesViewController : NSViewController <MTKViewDelegate>
- (void)shutdown;
- (void)showPreviewNativeUI;
- (BOOL)trayModeEnabled;
- (BOOL)saveTrayModeEnabled:(BOOL)enabled;
- (void)tickApplication:(NSTimer*)timer;
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
    NSTimer*              _logicTimer;
    int                   _previewFrameCount;
    bool                  _previewMode;
    std::string           _previewScenario;
    std::unique_ptr<MacOSContextMenuController> _contextMenus;
    id                    _sleepActivity;

    EmbeddedFileInterface _fileInterface;

    parties::rml::ElementRegistry _elementRegistry;

    AppCore _core;

    SoundPlayer _soundPlayer;

    std::unique_ptr<ScreenCaptureMac> _capturer;
    std::unique_ptr<ScreenCaptureMac> _audioCapturer;
    std::unique_ptr<VideoEncoderMac>  _encoder;
    bool                              _sharing;
    bool                              _encoderReady;
    bool                              _needsKeyframe;
    bool                              _nativePickerActive;
    uint32_t                          _encodeWidth;
    uint32_t                          _encodeHeight;

    parties::OpusCodec                _streamAudioEncoder;
    bool                              _streamAudioReady;
    std::vector<float>                _audioBuf;
    size_t                            _audioPos;
    uint8_t                           _opusBuf[parties::audio::MAX_OPUS_PACKET];

    std::unique_ptr<VideoDecoderIOS>  _decoder;
    bool                              _streamRevealed;

    PartiesPipHost*                   _pipHost;
    VideoFrameRouter                  _videoRouter;

    uint32_t _fpsFrameCount;
    std::chrono::steady_clock::time_point _fpsLastUpdate;
}

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

    Rml::LoadFontFace("ui/fonts/Inter-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Medium.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Bold.ttf");
    Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf", true);
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

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

    NSString* appSupport = [NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
    NSString* dir = [appSupport stringByAppendingPathComponent:@"Parties"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
    NSString* dbPath = [dir stringByAppendingPathComponent:@"parties.db"];

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

    bridge.open_share_picker = [bself]() { [bself startNativeShare]; };
    bridge.open_audio_share_picker = [bself]() { [bself startNativeAudioShare]; };

    bridge.stop_screen_share = [bself]() { [bself stopScreenShare]; };
    bridge.stop_audio_share = [bself]() { [bself stopAudioShare]; };

    bridge.request_keyframe = [bself]() {
        bself->_needsKeyframe = true;
    };

    bridge.stream_awaiting_keyframe = [bself](UserId id) {
        return bself->_core.viewing_sharer_ == id && bself->_core.awaiting_keyframe_;
    };

    bridge.clear_video_element = []() {};

    bridge.show_pip = [bself](UserId, const std::string& title) {
        [bself showPictureInPicture:title];
    };
    bridge.hide_pip = [bself](UserId, PipCloseReason) {
        [bself->_pipHost hide];
        [bself syncVideoRouter];
    };
    bridge.show_main_window = [bself]() {
        NSWindow* window = bself->_metalView.window;
        [NSApp unhide:nil];
        if (window.miniaturized) [window deminiaturize:nil];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    };

    // A call produces no input events, so macOS dims and powers down the display
    // while the user is still talking. An NSProcessInfo activity suppresses the
    // idle-display and idle-system timers for as long as the token is held, and
    // needs no extra framework. Re-assertions are a no-op while one is held.
    bridge.set_keep_awake = [bself](bool keep_awake) {
        if (keep_awake) {
            if (bself->_sleepActivity) return;
            bself->_sleepActivity = [[[NSProcessInfo processInfo]
                beginActivityWithOptions:(NSActivityUserInitiated |
                                          NSActivityIdleDisplaySleepDisabled)
                                  reason:@"Voice call in progress"] retain];
        } else if (bself->_sleepActivity) {
            [[NSProcessInfo processInfo] endActivity:bself->_sleepActivity];
            [bself->_sleepActivity release];
            bself->_sleepActivity = nil;
        }
    };

    if (_previewMode) {
        if (!_core.server_model_.init(_rmlContext) ||
            !_core.model_.init(_rmlContext) ||
            !_core.chat_model_.init(_rmlContext)) {
            NSLog(@"[Parties] Failed to initialize macOS UI fixture models");
            return;
        }
        PopulateUIFixture(_core, _previewScenario, true);
        NSLog(@"[Parties] Loaded macOS UI fixture: %s", _previewScenario.c_str());
    } else {
        if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
            NSLog(@"[Parties] AppCore init failed");
            return;
        }
        _coreInitialized = true;

        [self installMacOSModelCallbacks];

        _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
            [bself onVideoFrameData:sender_id data:data len:len];
        };

        std::string hostname = NSProcessInfo.processInfo.hostName.UTF8String;
        _core.load_or_generate_identity(hostname);
        _core.load_saved_prefs();
        _core.refresh_server_list();

        _logicTimer = [[NSTimer timerWithTimeInterval:1.0 / 60.0
                                             target:self
                                           selector:@selector(tickApplication:)
                                           userInfo:nil
                                            repeats:YES] retain];
        [[NSRunLoop mainRunLoop] addTimer:_logicTimer forMode:NSRunLoopCommonModes];
    }

    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->SetClass("platform-macos", true);
        _doc->SetClass("platform-desktop", true);
        _doc->Show();
        if (_previewMode) {
            ApplyUIFixtureDocument(_doc, _previewScenario);
        }
    }

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

- (BOOL)trayModeEnabled
{
    return _coreInitialized && _core.settings_.get_pref("window.tray_mode").value_or("1") != "0";
}

- (BOOL)saveTrayModeEnabled:(BOOL)enabled
{
    if (!_coreInitialized)
        return NO;
    if (_core.settings_.set_pref("window.tray_mode", enabled ? "1" : "0"))
        return YES;
    NSLog(@"[Parties] Failed to save tray mode preference");
    return NO;
}

- (void)tickApplication:(NSTimer*)timer
{
    if (!_coreInitialized)
        return;
    _core.tick();
    [self syncVideoRouter];
    BOOL visible = _metalView.window.visible && !_metalView.window.miniaturized && !NSApp.hidden;
    if (!visible && !_metalView.paused && _contextMenus)
        _contextMenus->Close();
    if (_metalView.paused == visible)
        _metalView.paused = !visible;
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_backendInitialized || !_rmlContext || !_commandQueue) return;

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

    if (_previewMode && _doc && ++_previewFrameCount == 60) {
        AuditUIControlLayout(_doc, _previewScenario.c_str());
        NSString* scenario = [NSString stringWithUTF8String:_previewScenario.c_str()];
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            NSLog(@"[Parties] %s macOS UI fixture: %@",
                completed.status == MTLCommandBufferStatusCompleted ? "Ready" : "Failed to render", scenario);
            dispatch_async(dispatch_get_main_queue(), ^{ [self showPreviewNativeUI]; });
        }];
    }
    [buffer presentDrawable:view.currentDrawable];
    [buffer commit];
}

- (void)installMacOSModelCallbacks
{
    PartiesViewController* bself = self;

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

    _core.model_.on_start_native_share = [bself]() {
        [bself startNativeShare];
    };

    _core.model_.on_select_share_target = nullptr;

    _core.model_.on_watch_sharer = [bself](int id) {
        [bself watchSharer:static_cast<UserId>(id)];
    };
    _core.model_.on_select_sharer = [bself](int id) {
        [bself watchSharer:static_cast<UserId>(id)];
    };
    _core.model_.on_toggle_watch = [bself](int id) {
        [bself watchSharer:static_cast<UserId>(id)];
    };
    _core.model_.on_stop_watching = [bself]() {
        [bself stopWatching];
    };
}

- (void)onVideoFrameData:(uint32_t)sender_id data:(const uint8_t*)data len:(size_t)len
{
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
    if (!_streamRevealed) {
        _streamRevealed = true;
        _core.model_.dirty("viewing_sharer_id");
    }
    _core.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

    if (!_doc) return;
    [self syncVideoRouter];
    auto* el = _videoRouter.target(_core.viewing_sharer_.load(), [self videoSurfaces]);
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

    _core.reset_video_sender(static_cast<uint32_t>(_core.model_.share_bitrate * 1000000.0f));

    PartiesViewController* bself = self;
    _capturer->on_frame = [bself](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        if (!bself->_encoder) return;

        if (!bself->_encoderReady) {
            uint32_t bitrate = (uint32_t)(bself->_core.model_.share_bitrate * 1000000.0f);
            static const uint32_t fps_table[] = { 15, 30, 60, 120 };
            uint32_t fps = fps_table[std::min(bself->_core.model_.share_fps.get(), 3)];

            MacVideoCodec mac_codec = (bself->_core.model_.share_codec == 0) ? MacVideoCodec::AV1
                                    : (bself->_core.model_.share_codec == 2) ? MacVideoCodec::H264
                                                                             : MacVideoCodec::H265;

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

                uint16_t fw = (uint16_t)bself->_encodeWidth;
                uint16_t fh = (uint16_t)bself->_encodeHeight;

                auto r = bself->_core.send_video_frame(data, len, is_kf, fw, fh, wire_codec);
                bself->_core.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

                if (bself->_core.viewing_sharer_ == bself->_core.user_id_ && bself->_decoder) {
                    VideoFrameHeader hdr;
                    hdr.frame_seq = r.frame_seq;
                    hdr.timestamp = r.frame_seq;
                    hdr.flags     = is_kf ? VIDEO_FLAG_KEYFRAME : 0;
                    hdr.width     = fw;
                    hdr.height    = fh;
                    hdr.codec     = static_cast<uint8_t>(wire_codec);

                    std::vector<uint8_t> preview(VIDEO_FRAME_HEADER_SIZE + len);
                    hdr.write(preview.data());
                    std::memcpy(preview.data() + VIDEO_FRAME_HEADER_SIZE, data, len);
                    [bself onVideoFrameData:bself->_core.user_id_
                                       data:preview.data()
                                        len:preview.size()];
                }
            };

            dispatch_async(dispatch_get_main_queue(), ^{
                bself->_sharing = true;
                bself->_core.model_.is_sharing = true;
                uint8_t payload[5] = {};
                bself->_core.net_.send_message(ControlMessageType::SCREEN_SHARE_START, payload, sizeof(payload));

                [bself watchSharer:bself->_core.user_id_];
            });
        }

        if (auto adapted = bself->_core.take_video_bitrate_update())
            bself->_encoder->set_bitrate(*adapted);
        bool forceKF = bself->_needsKeyframe;
        bself->_needsKeyframe = false;
        if (bself->_core.take_video_keyframe_request()) forceKF = true;

        if (!bself->_core.video_admit_frame()) {
            if (forceKF) bself->_needsKeyframe = true;
            return;
        }

        bself->_encoder->encode(buf, forceKF);
    };

    static constexpr int kAudioFrameSize = 960;
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

- (void)watchSharer:(UserId)sharerId
{
    _core.viewing_sharer_   = sharerId;
    _core.awaiting_keyframe_ = true;
    _decoder = std::make_unique<VideoDecoderIOS>();

    [self sendPLI:sharerId];

    uint32_t id32 = static_cast<uint32_t>(sharerId);
    _core.net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                             (const uint8_t*)&id32, sizeof(id32));

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

- (void)showPictureInPicture:(const std::string&)title
{
    if (!_pipHost) {
        PartiesViewController* bself = self;
        MacPipActions actions;
        actions.return_to_main = [bself]() { bself->_core.return_from_pip(); };
        actions.set_volume = [bself](float volume) { bself->_core.set_stream_volume(volume); };
        actions.close = [bself]() { bself->_core.close_pip(PipCloseReason::UserClosed); };
        actions.volume = [bself]() { return bself->_core.model_.stream_volume.get(); };
        actions.load_geometry = [bself]() -> std::optional<PipRect> {
            const auto saved = bself->_core.settings_.get_pref("window.pip_rect");
            return saved ? pip_rect_from_string(*saved) : std::nullopt;
        };
        actions.save_geometry = [bself](const PipRect& rect) {
            if (rect.width > 0 && rect.height > 0)
                bself->_core.settings_.set_pref("window.pip_rect", pip_rect_to_string(rect));
        };
        _pipHost = [[PartiesPipHost alloc] initWithDevice:_metalView.device actions:std::move(actions)];
    }
    if (![_pipHost showWithTitle:title]) {
        PartiesViewController* bself = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            bself->_core.close_pip(PipCloseReason::UserClosed);
        });
        return;
    }
    [self syncVideoRouter];
}

- (VideoFrameRouter::Surfaces)videoSurfaces
{
    VideoFrameRouter::Surfaces surfaces;
    surfaces.grid = _doc ? _doc->GetElementById("stream-grid") : nullptr;
    surfaces.pip = _pipHost ? [_pipHost videoSurface] : nullptr;
    return surfaces;
}

// Clears the surface a stream leaves when PiP changes. Metal keeps textures
// referenced by in-flight command buffers alive itself, so the release done
// by VideoElement::Clear is immediately safe.
- (void)syncVideoRouter
{
    _videoRouter.sync(_core.pip_.stream(), [self videoSurfaces]);
}

- (void)sendPLI:(UserId)targetId
{
    _core.send_pli(targetId);
}

- (void)shutdown
{
    [_logicTimer invalidate];
    [_logicTimer release];
    _logicTimer = nil;
    _metalView.paused = YES;
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
    [_pipHost prepareShutdown];
    if (_rmlContext) {
        Rml::RemoveContext(_rmlContext->GetName());
        _rmlContext = nullptr;
        _metalView.rmlContext = nullptr;
    }
    if (_rmlInitialized) {
        Rml::Shutdown();
        _rmlInitialized = false;
    }
    // The PiP renderer may only go after Rml::Shutdown released its manager.
    [_pipHost shutdown];
    [_pipHost release];
    _pipHost = nil;
    if (_backendInitialized) {
        Backend::Shutdown();
        _backendInitialized = false;
    }
    [_commandQueue release];
    _commandQueue = nil;
}

@end

@implementation PartiesAppDelegate {
    NSWindow*              _window;
    PartiesViewController* _viewController;
    PartiesStatusItemController* _statusItemController;
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
    if (_previewMode) {
        NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
        NSUInteger sizeIndex = [arguments indexOfObject:@"--ui-fixture-size"];
        if (sizeIndex != NSNotFound && sizeIndex + 2 < arguments.count) {
            frame.size.width = MAX(800, arguments[sizeIndex + 1].doubleValue);
            frame.size.height = MAX(500, arguments[sizeIndex + 2].doubleValue);
        }
    }
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled
      | NSWindowStyleMaskClosable
      | NSWindowStyleMaskMiniaturizable
      | NSWindowStyleMaskResizable;

    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility            = NSWindowTitleHidden;
    _window.styleMask                 |= NSWindowStyleMaskFullSizeContentView;
    _window.contentViewController      = _viewController;
    _window.releasedWhenClosed         = NO;
    _window.minSize                    = NSMakeSize(800, 500);
    if (_previewMode) [_window setContentSize:frame.size];

    [_window center];
    [_window makeKeyAndOrderFront:nil];

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];

    if (!_previewMode) {
        PartiesViewController* controller = _viewController;
        _statusItemController = [[PartiesStatusItemController alloc]
            initWithWindow:_window
                   enabled:[controller trayModeEnabled]
                  saveMode:[controller](bool enabled) {
                      return [controller saveTrayModeEnabled:enabled] == YES;
                  }];
    }

    [self installMainMenu];

#ifdef SPARKLE_ENABLED
    if (!_previewMode) {
        macos_updater_init();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(4 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            macos_updater_check_in_background();
        });
    }
#endif
}

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

    if (_statusItemController) {
        [appMenu addItem:[_statusItemController trayModeMenuItem]];
        [appMenu addItem:[NSMenuItem separatorItem]];
    }

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

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)hasVisibleWindows
{
    // A visible picture-in-picture panel counts as a visible window, so judge
    // the main window itself instead of hasVisibleWindows.
    const BOOL mainVisible = _window.visible && !_window.miniaturized && !NSApp.hidden;
    if (!mainVisible && _statusItemController)
        [_statusItemController showWindow:nil];
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
    [_statusItemController release];
    _statusItemController = nil;
    [_viewController shutdown];
    if (_quicInitialized) {
        parties::quic_cleanup();
        _quicInitialized = false;
    }
}

@end
