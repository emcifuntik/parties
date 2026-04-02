// iOS application — UIKit shell driving AppCore.
//
// Mirrors the role of app_macos.mm using UIKit instead of AppKit.
// MTKView hosts the Metal-backed RmlUI context; AppCore handles all
// networking, auth, audio, and model logic.

#import "AppDelegate.h"

#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>

// RmlUi Metal backend
#import "RmlUi_Backend_iOS_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/PropertyDefinition.h>
#ifdef RMLUI_DEBUG
#include <RmlUi/Debugger.h>
#endif

// Parties shared code
#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/audio_common.h>
#include <parties/video_common.h>
#include <parties/quic_common.h>

#include <client/app_core.h>
#include <client/sound_player.h>
#include <client/rmlui_backend.h>
#include <client/video_element.h>
#include <client/level_meter_element.h>
#include <client/gradient_circle_element.h>

#include <encdec/apple/VideoDecoderIOS.h>

#include <memory>
#include <cstring>

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

// ── Keyboard proxy (UIKeyInput → RmlUi key events) ──────────────────────────

@interface RmlKeyInput : UIView <UIKeyInput>
@property (nonatomic, assign) Rml::Context* rmlContext;
@end

@implementation RmlKeyInput
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }
- (void)insertText:(NSString*)text {
    if (!_rmlContext) return;
    if ([text isEqualToString:@"\n"]) {
        _rmlContext->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
        _rmlContext->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
        [self resignFirstResponder];
        return;
    }
    if (text.length > 0)
        _rmlContext->ProcessTextInput(Rml::String(text.UTF8String));
}
- (void)deleteBackward {
    if (!_rmlContext) return;
    _rmlContext->ProcessKeyDown(Rml::Input::KI_BACK, 0);
    _rmlContext->ProcessKeyUp(Rml::Input::KI_BACK, 0);
}
- (UITextAutocorrectionType)autocorrectionType  { return UITextAutocorrectionTypeNo; }
- (UITextAutocapitalizationType)autocapitalizationType { return UITextAutocapitalizationTypeNone; }
- (UITextSpellCheckingType)spellCheckingType     { return UITextSpellCheckingTypeNo; }
@end

// ── PartiesViewController ────────────────────────────────────────────────────

#import "PartiesViewController.h"

@implementation PartiesViewController {
    // Metal / RmlUi
    MTKView*                _view;
    id<MTLCommandQueue>     _commandQueue;
    Rml::Context*           _rmlContext;
    Rml::ElementDocument*   _doc;
    CGFloat                 _dpRatio;

    // Embedded file interface (must outlive RmlUi)
    EmbeddedFileInterface   _fileInterface;

    // Custom element instancers
    VideoElementInstancer   _videoInstancer;
    LevelMeterInstancer     _levelMeterInstancer;
    GradientCircleInstancer _gradientCircleInstancer;
    LevelMeterElement*      _levelMeter;

    // Touch / scroll (channels list)
    Rml::Element*           _channelsEl;
    CGPoint                 _touchStart;
    CGPoint                 _touchLast;
    BOOL                    _isScrolling;
    BOOL                    _isDraggingWidget;  // touched a slider — inhibit scroll
    float                   _velocityY;
    BOOL                    _momentumActive;
    double                  _lastMoveTime;
    double                  _lastFrameTime;

    // Keyboard proxy
    RmlKeyInput*            _keyInput;

    // Edit menu (paste/copy) — shown on long-press in focused input
    UIEditMenuInteraction*  _editMenuInteraction API_AVAILABLE(ios(16.0));

    // Safe area
    UIEdgeInsets            _safeInsets;
    int                     _viewportTopPx;

    // AppCore — all shared logic
    AppCore                 _core;
    SoundPlayer             _soundPlayer;

    // Video decoder (receive screen shares)
    std::unique_ptr<VideoDecoderIOS> _decoder;
    bool                    _streamRevealed;
    uint32_t                _streamWidth;
    uint32_t                _streamHeight;
    bool                    _streamFullscreen;

    // FPS counter
    uint32_t _fpsFrameCount;
    std::chrono::steady_clock::time_point _fpsLastUpdate;
}

// ── View setup ───────────────────────────────────────────────────────────────

- (void)loadView
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _view = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:device];
    _view.colorPixelFormat        = MTLPixelFormatBGRA8Unorm;
    _view.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    _view.clearColor              = MTLClearColorMake(0.059, 0.067, 0.090, 1.0); // #0F1117
    _view.clearStencil            = 0;
    _view.delegate                = self;
    _view.preferredFramesPerSecond = 60;
    self.view = _view;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    // ── Audio session — keep audio alive while locked / in background ────
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker |
                         AVAudioSessionCategoryOptionAllowBluetooth |
                         AVAudioSessionCategoryOptionMixWithOthers
                   error:&err];
    if (err) NSLog(@"[Parties] Audio session setCategory error: %@", err);
    [session setActive:YES error:&err];
    if (err) NSLog(@"[Parties] Audio session setActive error: %@", err);

    [session requestRecordPermission:^(BOOL granted) {
        if (!granted)
            NSLog(@"[Parties] Microphone permission denied — voice chat will not work.");
    }];

    // ── Metal + RmlUi ────────────────────────────────────────────────────
    _commandQueue = [_view.device newCommandQueue];
    Backend::Initialize(_view.device, _view);

    Rml::SetFileInterface(&_fileInterface);
    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());
    Rml::Initialise();

    // Register window-action as no-op to suppress warnings (used on Windows for caption hit-testing)
    Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
        .AddParser("keyword", "none, caption, close, minimize, maximize");

    Rml::Factory::RegisterElementInstancer("video_frame", &_videoInstancer);
    Rml::Factory::RegisterElementInstancer("level_meter", &_levelMeterInstancer);
    Rml::Factory::RegisterElementInstancer("gradient_circle", &_gradientCircleInstancer);

    _dpRatio = UIScreen.mainScreen.scale;
    CGSize native = UIScreen.mainScreen.nativeBounds.size;
    int physW = (int)native.width;
    int physH = (int)native.height;
    Backend::SetViewport(physW, physH);

    _rmlContext = Rml::CreateContext("main", Rml::Vector2i(physW, physH));
    _rmlContext->SetDensityIndependentPixelRatio((float)_dpRatio);

    // Keyboard proxy.
    _keyInput = [[RmlKeyInput alloc] initWithFrame:CGRectMake(0, -2, 1, 1)];
    _keyInput.rmlContext = _rmlContext;
    [_view addSubview:_keyInput];

    // Edit menu (paste/copy) — long-press shows system edit menu over focused input.
    if (@available(iOS 16.0, *)) {
        _editMenuInteraction = [[UIEditMenuInteraction alloc] initWithDelegate:self];
        [_view addInteraction:_editMenuInteraction];
        UILongPressGestureRecognizer* lp =
            [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
        lp.minimumPressDuration = 0.5;
        [_view addGestureRecognizer:lp];
    }

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(keyboardWillShow:)
        name:UIKeyboardWillShowNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(keyboardWillHide:)
        name:UIKeyboardWillHideNotification object:nil];

    // Fonts — loaded via embedded file interface
    Rml::LoadFontFace("ui/fonts/Inter-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Medium.ttf");
    Rml::LoadFontFace("ui/fonts/Inter-Bold.ttf");

#ifdef RMLUI_DEBUG
    Rml::Debugger::Initialise(_rmlContext);
    Rml::Debugger::SetVisible(false);
    UITapGestureRecognizer* dbgTap =
        [[UITapGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(toggleDebugger)];
    dbgTap.numberOfTouchesRequired = 4;
    [_view addGestureRecognizer:dbgTap];
#endif

    // ── Sound player ─────────────────────────────────────────────────────
    _soundPlayer.init();

    // ── Settings path ────────────────────────────────────────────────────
    NSString* docs = [NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString* dbPath = [docs stringByAppendingPathComponent:@"parties.db"];

    // ── Build PlatformBridge ─────────────────────────────────────────────
    PartiesViewController* bself = self;

    PlatformBridge bridge;

    bridge.copy_to_clipboard = [](const std::string& text) {
        [UIPasteboard generalPasteboard].string =
            [NSString stringWithUTF8String:text.c_str()];
    };

    bridge.play_sound = [bself](SoundPlayer::Effect e) {
        bself->_soundPlayer.play(e);
    };
    bridge.set_notification_volume = [bself](float v) {
        bself->_soundPlayer.set_volume(v);
    };

    bridge.show_channel_menu = nullptr;  // no right-click on iOS
    bridge.show_server_menu  = nullptr;

    bridge.open_share_picker = nullptr;  // iOS: receive-only, no screen share send

    bridge.on_authenticated = [bself]() {
        bself->_core.net_.open_av_streams();
    };

    bridge.stop_screen_share = nullptr;  // iOS doesn't send screen shares

    bridge.request_keyframe = nullptr;

    bridge.clear_video_element = [bself]() {
        if (!bself->_doc) return;
        auto* el = dynamic_cast<VideoElement*>(bself->_doc->GetElementById("screen-share"));
        if (el) el->Clear();
    };

    // iOS manages the decoder directly in watchSharer:/stopWatching:
    // (same pattern as macOS) — no start/stop_decode_thread needed.
    bridge.start_decode_thread = nullptr;
    bridge.stop_decode_thread  = nullptr;

    // ── Init QUIC ─────────────────────────────────────────────────────────
    if (!parties::quic_init()) {
        NSLog(@"[Parties] MsQuic init failed");
        return;
    }

    // ── Init AppCore ─────────────────────────────────────────────────────
    if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
        NSLog(@"[Parties] AppCore init failed");
        return;
    }

    // ── iOS-specific model callbacks ─────────────────────────────────────
    [self installIOSModelCallbacks];

    // ── Wire video frame reception to local decoder ──────────────────────
    _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
        [bself onVideoFrameData:sender_id data:data len:len];
    };

    // ── Load identity ────────────────────────────────────────────────────
    NSString* deviceName = [[UIDevice currentDevice] name];
    _core.load_or_generate_identity(std::string(deviceName.UTF8String));

    // ── Load saved audio prefs ───────────────────────────────────────────
    _core.load_saved_prefs();

    // ── Populate server list ─────────────────────────────────────────────
    _core.refresh_server_list();

    // ── UI document ──────────────────────────────────────────────────────
    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->Show();
        _doc->SetClass("platform-ios", true);
        _levelMeter = static_cast<LevelMeterElement*>(_doc->GetElementById("voice-level-meter"));
    }

    _channelsEl = _doc ? _doc->GetElementById("channels") : nullptr;
}

// ── iOS-specific model callback overrides ────────────────────────────────────

- (void)installIOSModelCallbacks
{
    PartiesViewController* bself = self;

    // iOS: no screen share sending — disable toggle
    _core.model_.on_toggle_share = nullptr;
    _core.model_.on_start_native_share = nullptr;
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

    // iOS: single tap toggles fullscreen + landscape rotation
    _core.model_.on_stream_tap_fullscreen = [bself]() {
        [bself toggleStreamFullscreen];
    };
}

// ── Video frame routing (VideoToolbox decoder) ───────────────────────────────

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
            NSLog(@"[Video] Decoder init failed (codec=%u, %ux%u)", codec_id, w, h);
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
    _streamWidth  = w;
    _streamHeight = h;

    const uint8_t* y_plane  = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
    const uint8_t* uv_plane = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 1);
    uint32_t y_stride  = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
    uint32_t uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 1);

    el->UpdateNV12Frame(y_plane, y_stride, uv_plane, uv_stride, w, h);

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
}

- (void)watchSharer:(UserId)uid
{
    _core.viewing_sharer_   = uid;
    _core.awaiting_keyframe_ = true;
    _decoder = std::make_unique<VideoDecoderIOS>();
    // on_decoded is wired lazily in onVideoFrameData on first frame

    _core.send_pli(uid);

    uint32_t id32 = static_cast<uint32_t>(uid);
    _core.net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                            (const uint8_t*)&id32, sizeof(id32));

    _core.model_.viewing_sharer_id = static_cast<int>(uid);
    _streamRevealed = false;
    // Don't dirty yet — onVideoDecoded dirties on first frame to avoid black flash
}

- (void)toggleStreamFullscreen
{
    _streamFullscreen = !_streamFullscreen;
    _core.model_.stream_fullscreen = _streamFullscreen;
    _core.model_.dirty("stream_fullscreen");

    if (_streamFullscreen && _streamWidth > _streamHeight) {
        // Landscape video → rotate to landscape
        [self setNeedsUpdateOfSupportedInterfaceOrientations];
        auto scene = self.view.window.windowScene;
        if (scene) {
            auto prefs = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeRight];
            [scene requestGeometryUpdateWithPreferences:prefs
                errorHandler:^(NSError* error) {
                    NSLog(@"[Parties] Orientation change failed: %@", error);
                }];
        }
    } else if (!_streamFullscreen) {
        // Exit fullscreen → back to portrait
        [self setNeedsUpdateOfSupportedInterfaceOrientations];
        auto scene = self.view.window.windowScene;
        if (scene) {
            auto prefs = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskPortrait];
            [scene requestGeometryUpdateWithPreferences:prefs
                errorHandler:^(NSError* error) {
                    NSLog(@"[Parties] Orientation change failed: %@", error);
                }];
        }
    }
}

- (void)stopWatching
{
    // Exit fullscreen if active
    if (_streamFullscreen) {
        _streamFullscreen = false;
        _core.model_.stream_fullscreen = false;
        _core.model_.dirty("stream_fullscreen");
        [self setNeedsUpdateOfSupportedInterfaceOrientations];
        auto scene = self.view.window.windowScene;
        if (scene) {
            auto prefs = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskPortrait];
            [scene requestGeometryUpdateWithPreferences:prefs
                errorHandler:^(NSError* error) {}];
        }
    }

    _core.viewing_sharer_   = 0;
    _decoder.reset();
    _core.awaiting_keyframe_ = false;
    _streamRevealed = false;
    _streamWidth = _streamHeight = 0;

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

// ── Layout / orientation ──────────────────────────────────────────────────────

- (void)updateViewportSize
{
    _safeInsets = self.view.safeAreaInsets;
    _viewportTopPx = (int)(_safeInsets.top * _dpRatio);
    Backend::SetViewportTopOffset(_viewportTopPx);

    // Use view bounds (respects current orientation) instead of
    // nativeBounds (always portrait).
    CGSize pts = self.view.bounds.size;
    int physW = (int)(pts.width  * _dpRatio);
    int physH = (int)(pts.height * _dpRatio) - _viewportTopPx;
    Backend::SetViewport(physW, physH);
    if (_rmlContext)
        _rmlContext->SetDimensions(Rml::Vector2i(physW, physH));

    [self applySafeAreaToDocument];
}

- (void)viewSafeAreaInsetsDidChange
{
    [super viewSafeAreaInsetsDidChange];
    [self updateViewportSize];
}

- (void)viewWillTransitionToSize:(CGSize)size
       withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    PartiesViewController* bself = self;
    [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext>) {
        [bself updateViewportSize];
        bself->_view.frame = bself.view.bounds;
    } completion:nil];
}

- (void)applySafeAreaToDocument
{
    if (!_doc) return;

    Rml::Element* body = nullptr;
    for (int i = 0; i < _doc->GetNumChildren(); i++) {
        Rml::Element* child = _doc->GetChild(i);
        if (child && child->GetTagName() == "body") { body = child; break; }
    }
    if (!body) return;

    auto toDp = [](CGFloat pt) -> Rml::String {
        char buf[32]; snprintf(buf, sizeof(buf), "%.0fdp", (double)pt); return buf;
    };
    body->SetProperty("padding-top",    "0dp");
    body->SetProperty("padding-bottom", toDp(_safeInsets.bottom));
    body->SetProperty("padding-left",   toDp(_safeInsets.left));
    body->SetProperty("padding-right",  toDp(_safeInsets.right));
}

// ── Keyboard avoidance ───────────────────────────────────────────────────────

- (void)keyboardWillShow:(NSNotification*)note
{
    CGRect kb     = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect screen = UIScreen.mainScreen.bounds;
    NSTimeInterval dur = [note.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];

    CGFloat shiftY = 0;
    if (_rmlContext) {
        Rml::Element* focused = _rmlContext->GetFocusElement();
        if (focused) {
            float bottomPt = (focused->GetAbsoluteTop() + focused->GetClientHeight())
                              / (float)_dpRatio;
            if (bottomPt > screen.size.height / 2.0f)
                shiftY = -kb.size.height;
        }
    }
    [UIView animateWithDuration:dur animations:^{
        self->_view.frame = CGRectMake(0, shiftY, screen.size.width, screen.size.height);
    }];
}

- (void)keyboardWillHide:(NSNotification*)note
{
    CGRect screen = UIScreen.mainScreen.bounds;
    NSTimeInterval dur = [note.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    [UIView animateWithDuration:dur animations:^{
        self->_view.frame = screen;
    }];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    if (_streamFullscreen && _streamWidth > _streamHeight)
        return UIInterfaceOrientationMaskLandscapeRight | UIInterfaceOrientationMaskLandscapeLeft;
    return UIInterfaceOrientationMaskPortrait;
}

// ── Teardown ─────────────────────────────────────────────────────────────────

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    if (self.presentedViewController) return;

    _core.shutdown();

    if (_decoder) {
        _decoder->shutdown();
        _decoder.reset();
    }

    if (_rmlContext) {
        Rml::RemoveContext(_rmlContext->GetName());
        _rmlContext = nullptr;
    }
#ifdef RMLUI_DEBUG
    Rml::Debugger::Shutdown();
#endif
    Rml::Shutdown();
    Backend::Shutdown();
    parties::quic_cleanup();
}

#ifdef RMLUI_DEBUG
- (void)toggleDebugger
{
    Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
}
#endif

// ── MTKViewDelegate ──────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    int w = (int)size.width;
    int h = (int)size.height - _viewportTopPx;
    Backend::SetViewport(w, h);
    if (_rmlContext)
        _rmlContext->SetDimensions(Rml::Vector2i(w, h));
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_rmlContext) return;

    // Tick shared logic (network, audio levels, FPS counter, etc.)
    _core.tick();

    // Update voice level meter
    if (_levelMeter && _core.model_.is_connected) {
        _levelMeter->SetLevel(audio::rms_to_perceptual(_core.audio_.voice_level()));
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

    id<MTLCommandBuffer> cmd = [_commandQueue commandBuffer];

    // Scroll momentum.
    if (_momentumActive) {
        double now = CACurrentMediaTime();
        float  dt  = (float)(now - _lastFrameTime);
        _lastFrameTime = now;
        if (dt > 0.0f && dt < 0.1f) {
            float delta = _velocityY * dt * 0.015f;
            _rmlContext->ProcessMouseWheel(Rml::Vector2f(0, delta), 0);
            _velocityY *= powf(0.998f, dt * 1000.0f);
            if (fabsf(_velocityY) < 30.0f) { _velocityY = 0.0f; _momentumActive = NO; }
        }
    }

    // Keyboard show/hide based on focused element.
    if (_keyInput) {
        Rml::Element* focused = _rmlContext->GetFocusElement();
        BOOL want = (focused && focused->GetTagName() == "input");
        if (want  && !_keyInput.isFirstResponder) [_keyInput becomeFirstResponder];
        if (!want &&  _keyInput.isFirstResponder) [_keyInput resignFirstResponder];
    }

    // Sync viewport to actual drawable.
    {
        id<MTLTexture> colorTex = pass.colorAttachments[0].texture;
        if (colorTex)
            Backend::SetViewport((int)colorTex.width, (int)colorTex.height - _viewportTopPx);
    }

    Backend::BeginFrame(cmd, pass);
    _rmlContext->Update();
    _rmlContext->Render();
    Backend::EndFrame();

    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];
}

// ── Touch input ──────────────────────────────────────────────────────────────

- (Rml::Vector2f)physFromPt:(CGPoint)p
{
    return { (float)(p.x * _dpRatio),
             (float)(p.y * _dpRatio) - (float)_viewportTopPx };
}

- (BOOL)hitTestSlider:(Rml::Vector2f)pt
{
    // Walk up from the hovered element — if any ancestor is a sliderbar, we're on a slider.
    Rml::Element* el = _rmlContext->GetHoverElement();
    while (el) {
        const Rml::String& tag = el->GetTagName();
        if (tag == "sliderbar" || tag == "slidertrack" ||
            (tag == "input" && el->GetAttribute("type") &&
             el->GetAttribute("type")->Get<Rml::String>() == "range"))
            return YES;
        el = el->GetParentNode();
    }
    return NO;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_rmlContext || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    _touchStart     = [touch locationInView:_view];
    _touchLast      = _touchStart;
    _isScrolling    = NO;
    _momentumActive = NO;
    _velocityY      = 0.0f;
    _lastMoveTime   = CACurrentMediaTime();
    Rml::Vector2f pt = [self physFromPt:_touchStart];
    _rmlContext->ProcessMouseMove((int)pt.x, (int)pt.y, 0);
    // Check if we hit a slider before pressing — if so, lock out scrolling.
    _isDraggingWidget = [self hitTestSlider:pt];
    // Only press immediately for drag-based widgets (sliders).
    // For everything else, defer until touchesEnded to avoid
    // toggling checkboxes when the user intends to scroll.
    if (_isDraggingWidget) {
        _rmlContext->ProcessMouseButtonDown(0, 0);
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_rmlContext || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    CGPoint  cur   = [touch locationInView:_view];

    float dyPt = (float)(cur.y - _touchLast.y);
    _touchLast = cur;

    if (!_isScrolling && !_isDraggingWidget) {
        float dy = fabsf((float)(cur.y - _touchStart.y));
        if (dy > 10.0f) {
            _isScrolling = YES;
        }
    }

    Rml::Vector2f pt = [self physFromPt:cur];
    _rmlContext->ProcessMouseMove((int)pt.x, (int)pt.y, 0);

    if (_isScrolling) {
        // Feed scroll delta to RmlUi (negate — same convention as macOS scrollWheel).
        float scrollDelta = -dyPt * 0.015f;
        _rmlContext->ProcessMouseWheel(Rml::Vector2f(0, scrollDelta), 0);

        double now = CACurrentMediaTime();
        double dt  = now - _lastMoveTime;
        _lastMoveTime = now;
        if (dt > 0.0 && dt < 0.1) {
            float sample = -dyPt / (float)dt;
            _velocityY = 0.6f * _velocityY + 0.4f * sample;
            _velocityY = std::clamp(_velocityY, -1500.0f, 1500.0f);
        }
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_rmlContext || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    CGPoint  cur   = [touch locationInView:_view];

    Rml::Vector2f pt = [self physFromPt:cur];
    _rmlContext->ProcessMouseMove((int)pt.x, (int)pt.y, 0);

    if (!_isScrolling) {
        if (_isDraggingWidget) {
            // Slider drag — button was pressed in touchesBegan, just release.
            _rmlContext->ProcessMouseButtonUp(0, 0);
        } else {
            // Tap — send down+up together now that we know it's not a scroll.
            _rmlContext->ProcessMouseButtonDown(0, 0);
            _rmlContext->ProcessMouseButtonUp(0, 0);
        }
        _velocityY = 0.0f;
    } else if (fabsf(_velocityY) > 50.0f) {
        _momentumActive = YES;
        _lastFrameTime = CACurrentMediaTime();
    }
    _isScrolling = NO;
    _isDraggingWidget = NO;
    _rmlContext->ProcessMouseLeave();
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    _isScrolling = NO;
    _isDraggingWidget = NO;
    if (_rmlContext) {
        _rmlContext->ProcessMouseButtonUp(0, 0);
        _rmlContext->ProcessMouseLeave();
    }
}

// ── Edit menu (paste / copy / select all) ────────────────────────────────────

- (BOOL)isInputFocused {
    if (!_rmlContext) return NO;
    Rml::Element* focused = _rmlContext->GetFocusElement();
    return focused && focused->GetTagName() == "input";
}

- (BOOL)canBecomeFirstResponder { return YES; }

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(paste:))     return [self isInputFocused] && [UIPasteboard generalPasteboard].hasStrings;
    if (action == @selector(copy:))      return [self isInputFocused];
    if (action == @selector(selectAll:)) return [self isInputFocused];
    return [super canPerformAction:action withSender:sender];
}

- (void)paste:(id)sender {
    NSString* str = [UIPasteboard generalPasteboard].string;
    if (str.length > 0 && _rmlContext)
        _rmlContext->ProcessTextInput(Rml::String(str.UTF8String));
}

- (void)copy:(id)sender {
    if (!_rmlContext) return;
    // Select all then copy via Ctrl+C (RmlUi uses Ctrl for clipboard shortcuts)
    _rmlContext->ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_CTRL);
    _rmlContext->ProcessKeyUp(Rml::Input::KI_A, Rml::Input::KM_CTRL);
    _rmlContext->ProcessKeyDown(Rml::Input::KI_C, Rml::Input::KM_CTRL);
    _rmlContext->ProcessKeyUp(Rml::Input::KI_C, Rml::Input::KM_CTRL);
}

- (void)selectAll:(id)sender {
    if (!_rmlContext) return;
    _rmlContext->ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_CTRL);
    _rmlContext->ProcessKeyUp(Rml::Input::KI_A, Rml::Input::KM_CTRL);
}

// Hardware keyboard shortcuts (Cmd+V, Cmd+C, Cmd+A)
- (NSArray<UIKeyCommand*>*)keyCommands {
    return @[
        [UIKeyCommand keyCommandWithInput:@"v" modifierFlags:UIKeyModifierCommand action:@selector(paste:)],
        [UIKeyCommand keyCommandWithInput:@"c" modifierFlags:UIKeyModifierCommand action:@selector(copy:)],
        [UIKeyCommand keyCommandWithInput:@"a" modifierFlags:UIKeyModifierCommand action:@selector(selectAll:)],
    ];
}

// Long-press → show system edit menu at touch point
- (void)handleLongPress:(UILongPressGestureRecognizer*)gesture {
    if (gesture.state != UIGestureRecognizerStateBegan) return;
    if (![self isInputFocused]) return;

    if (@available(iOS 16.0, *)) {
        CGPoint point = [gesture locationInView:_view];
        UIEditMenuConfiguration* config =
            [UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:point];
        [_editMenuInteraction presentEditMenuWithConfiguration:config];
    }
}

// UIEditMenuInteractionDelegate
- (UIMenu*)editMenuInteraction:(UIEditMenuInteraction*)interaction
          menuForConfiguration:(UIEditMenuConfiguration*)configuration
          suggestedActions:(NSArray<UIMenuElement*>*)suggestedActions
    API_AVAILABLE(ios(16.0))
{
    return nil;  // nil = use default system menu (Paste, Copy, Select All)
}

@end
