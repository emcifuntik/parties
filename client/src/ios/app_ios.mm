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
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
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
#include <client/gradient_circle_element.h>
#include <client/custom_elements.h>

#include <encdec/apple/VideoDecoderIOS.h>

#include <memory>
#include <cstring>

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

namespace {

enum class IOSScrollAxis {
    None,
    Horizontal,
    Vertical,
};

#ifndef NDEBUG
ChannelUser PreviewUser(const char* name, int id, bool speaking = false,
                        bool muted = false, bool streaming = false, int role = 3)
{
    ChannelUser user;
    user.name = name;
    user.id = id;
    user.role = role;
    user.speaking = speaking;
    user.muted = muted;
    user.streaming = streaming;
    return user;
}

ChatMessage PreviewMessage(int64_t id, int senderId, const char* sender,
                           const char* text, const char* time, int color,
                           bool own = false)
{
    ChatMessage message;
    message.id = id;
    message.sender_id = senderId;
    message.sender_name = sender;
    message.initials = Rml::String(sender).substr(0, (std::min)(size_t(2), std::strlen(sender)));
    message.text = text;
    message.timestamp_str = time;
    message.color_index = color;
    message.is_own = own;
    message.segments.push_back({text, false});
    return message;
}

bool IsIOSPreviewScenario(const std::string& scenario)
{
    static constexpr const char* scenarios[] = {
        "launcher", "sidebar", "party-modal", "onboarding", "onboarding-restore",
        "onboarding-key-import", "recovery", "room", "chat", "settings",
        "settings-screen-share", "settings-hotkeys", "settings-account",
        "stream-single", "streams", "member", "login", "tofu",
        "create-channel", "create-text-channel", "rename-channel",
        "global-name", "server-nickname"
    };
    for (const char* value : scenarios)
        if (scenario == value) return true;
    return false;
}

void PopulateIOSPreview(AppCore& core, const std::string& scenario)
{
    LobbyModel& lobby = core.model_;
    ServerListModel& servers = core.server_model_;
    ChatModel& chat = core.chat_model_;

    const bool disconnected = scenario == "launcher" || scenario == "party-modal" ||
        scenario == "onboarding" || scenario == "onboarding-restore" ||
        scenario == "onboarding-key-import" || scenario == "recovery" ||
        scenario == "login" || scenario == "tofu" ||
        scenario == "global-name" || scenario == "server-nickname";

    lobby.is_connected = !disconnected;
    lobby.server_name = "Night Shift";
    lobby.server_initials = "NS";
    lobby.username = "tuxick";
    lobby.current_channel = 1;
    lobby.current_channel_name = "General";
    lobby.ping_ms = 24;
    lobby.can_manage_channels = true;
    lobby.my_role = 0;
    lobby.voice_volume = 1.0f;
    lobby.secondary_volume = 0.8f;
    lobby.music_send_volume = 0.72f;
    lobby.notification_volume = 0.75f;
    lobby.denoise_enabled = true;
    lobby.vad_enabled = true;
    lobby.vad_threshold = 0.12f;
    lobby.share_bitrate = 8.0f;
    lobby.share_fps = 2;
    lobby.mobile_show_content = !disconnected && scenario != "sidebar";

    ChannelInfo general;
    general.id = 1;
    general.name = "General";
    general.max_users = 64;
    general.users = {
        PreviewUser("tuxick", 1, false, false, false, 0),
        PreviewUser("IceTroll", 2, true, false, false, 1),
        PreviewUser("ivan", 3, false, false, true),
        PreviewUser("Sara", 4), PreviewUser("Maks", 5, false, true),
        PreviewUser("android", 6), PreviewUser("Noah", 7)
    };
    general.user_count = static_cast<int>(general.users.size());
    ChannelInfo lounge;
    lounge.id = 2;
    lounge.name = "Late night";
    lounge.max_users = 12;
    lounge.users = {PreviewUser("Maya", 8), PreviewUser("Liam", 9, true)};
    lounge.user_count = static_cast<int>(lounge.users.size());
    ChannelInfo focus;
    focus.id = 3;
    focus.name = "Quiet focus";
    focus.max_users = 8;
    lobby.channels = Rml::Vector<ChannelInfo>{general, lounge, focus};

    lobby.capture_devices = Rml::Vector<AudioDevice>{{"iPhone Microphone", 0}};
    lobby.playback_devices = Rml::Vector<AudioDevice>{{"iPhone Speaker", 0}, {"AirPods", 1}};
    lobby.mute_key_name = "Not available on iOS";
    lobby.deafen_key_name = "Not available on iOS";
    lobby.ptt_key_name = "Not available on iOS";

    ServerEntry night;
    night.id = 1; night.name = "Night Shift"; night.initials = "NS";
    night.color_index = 1; night.host = "night.parties.local";
    night.online = true; night.users_text = "12 online · 5 in General";
    ServerEntry studio;
    studio.id = 2; studio.name = "Creative Studio"; studio.initials = "CS";
    studio.color_index = 2; studio.host = "studio.parties.local";
    studio.online = true; studio.users_text = "7 online · 3 in Lounge";
    ServerEntry guild;
    guild.id = 3; guild.name = "Old Guild"; guild.initials = "OG";
    guild.color_index = 3; guild.host = "guild.parties.local";
    guild.online = false; guild.locked = true;
    servers.servers = Rml::Vector<ServerEntry>{night, studio, guild};
    servers.party_count_text = "3 parties";
    servers.global_name = "tuxick";
    servers.connected_server_id = disconnected ? 0 : 1;
    servers.fingerprint = "5E7A 91C2 4D3F";
    servers.has_identity = true;
    servers.seed_phrase = "amber vessel orbit meadow copper velvet island echo marble lunar gentle harbor";

    chat.text_channels = Rml::Vector<TextChannel>{
        {1, "general", false}, {2, "clips-and-links", true}, {3, "off-topic", false}};
    chat.active_channel = scenario == "chat" ? 1 : 0;
    chat.active_channel_name = "general";
    chat.can_manage_channels = true;
    chat.messages = Rml::Vector<ChatMessage>{
        PreviewMessage(1, 2, "IceTroll", "Anyone up for a quick match later?", "20:41", 2),
        PreviewMessage(2, 3, "ivan", "I can join after I finish this build.", "20:43", 5),
        PreviewMessage(3, 4, "Sara", "Perfect. I pinned the server details above.", "20:44", 8),
        PreviewMessage(4, 1, "tuxick", "Give me ten minutes and I'll be there.", "20:45", 3, true)};

    lobby.router.reset();
    if (scenario == "chat")
        lobby.router.go(DocumentRoute::Chat);
    else if (scenario == "settings" || scenario == "settings-screen-share" ||
             scenario == "settings-hotkeys" || scenario == "settings-account") {
        lobby.router.go(DocumentRoute::Settings);
        SettingsSection section = SettingsSection::AudioVoice;
        if (scenario == "settings-screen-share") section = SettingsSection::ScreenShare;
        if (scenario == "settings-hotkeys") section = SettingsSection::Hotkeys;
        if (scenario == "settings-account") section = SettingsSection::AccountKeys;
        lobby.router.select_settings(section);
    }
    else if (scenario == "stream-single" || scenario == "streams") {
        lobby.router.go(DocumentRoute::Streams);
        lobby.someone_sharing = true;
        lobby.watching_count = scenario == "stream-single" ? 1 : 2;
        lobby.viewing_sharer_id = 2;
        lobby.stream_fps = 60;
        lobby.sharers = Rml::Vector<ActiveSharer>{{2, "IceTroll", true}, {3, "ivan", true}};
        lobby.watched = Rml::Vector<WatchedStream>{{2, "IceTroll · Counter-Strike 2", "screen-share-2"}};
        if (scenario == "streams")
            lobby.watched.silent().push_back({3, "ivan · Zen Browser", "screen-share-3"});
    }

    servers.show_onboarding = scenario == "onboarding" || scenario == "onboarding-restore" ||
        scenario == "onboarding-key-import" || scenario == "recovery";
    servers.onboarding_step = scenario == "recovery" ? 1 : 0;
    servers.show_restore = scenario == "onboarding-restore";
    servers.show_key_import = scenario == "onboarding-key-import";
    if (servers.show_onboarding.get()) servers.has_identity = false;
    servers.show_add_form = scenario == "party-modal";
    servers.edit_host = "voice.example.com";
    servers.edit_port = "7800";
    servers.edit_nickname = "";
    servers.show_login = scenario == "login";
    servers.login_status = "Secure connection ready";
    servers.login_show_username = scenario == "login";
    servers.login_username = "";
    servers.show_global_name_editor = scenario == "global-name";
    servers.global_name_input = "tuxick";
    servers.show_server_nickname_editor = scenario == "server-nickname";
    servers.server_nickname_server_id = 1;
    servers.server_nickname_server_name = "Night Shift";
    servers.server_nickname_input = "";
    servers.show_tofu_warning = scenario == "tofu";
    servers.tofu_fingerprint = "71:08:BB:6E:20:91:4F:3A";

    lobby.show_create_channel = scenario == "create-channel";
    lobby.show_rename_channel = scenario == "rename-channel";
    lobby.rename_channel_name = "General";
    lobby.new_rename_channel_name = "General lounge";
    chat.show_create_text_channel = scenario == "create-text-channel";

    lobby.show_user_menu = scenario == "member";
    if (scenario == "member") {
        lobby.menu_user_id = 2;
        lobby.menu_user_name = "IceTroll";
        lobby.menu_user_role = 1;
        lobby.menu_user_volume = 0.86f;
        lobby.menu_user_music_volume = 0.64f;
        lobby.menu_user_compress = true;
        lobby.menu_can_roles = true;
        lobby.menu_can_kick = true;
    }
    if (scenario == "room") {
        lobby.someone_sharing = true;
        lobby.sharers = Rml::Vector<ActiveSharer>{{3, "ivan", false}};
    }
    if (scenario == "settings-account") {
        lobby.show_private_key = true;
        lobby.identity_private_key = "8d99c2eed598508a94eb471d7334ee80d28a57139d1a7b7273e16105106fe0a4";
        lobby.show_import_identity = true;
    }
}
#endif

} // namespace

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
    bool                    _backendInitialized;
    bool                    _rmlInitialized;
    bool                    _debuggerInitialized;
    bool                    _coreInitialized;
    bool                    _quicInitialized;
    bool                    _previewMode;
    std::string             _previewScenario;

    // Embedded file interface (must outlive RmlUi)
    EmbeddedFileInterface   _fileInterface;

    // Custom element instancers
    parties::rml::ElementRegistry _elementRegistry;

    // Touch scrolling. The weak observer becomes null if data binding removes
    // the target while momentum is still active.
    Rml::ObserverPtr<Rml::Element> _scrollTarget;
    Rml::ObserverPtr<Rml::Element> _scrollCandidateX;
    Rml::ObserverPtr<Rml::Element> _scrollCandidateY;
    IOSScrollAxis           _scrollAxis;
    CGPoint                 _touchStart;
    CGPoint                 _touchLast;
    BOOL                    _isScrolling;
    BOOL                    _isDraggingWidget;  // touched a slider — inhibit scroll
    float                   _scrollVelocity; // physical pixels per second
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
    int                     _viewportHeightPx;
    CGFloat                 _keyboardInsetPt;

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
    _view.preferredFramesPerSecond = UIScreen.mainScreen.maximumFramesPerSecond;
    self.view = _view;
    [_view release];
    [device release];
}

- (void)viewDidLoad
{
    [super viewDidLoad];

#ifndef NDEBUG
    NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 0; i + 1 < arguments.count; ++i) {
        if ([arguments[i] isEqualToString:@"--ui-fixture"]) {
            _previewScenario = std::string(arguments[i + 1].UTF8String);
            _previewMode = IsIOSPreviewScenario(_previewScenario);
            if (!_previewMode)
                NSLog(@"[Parties] Unknown iOS UI fixture: %s", _previewScenario.c_str());
            break;
        }
    }
#endif

    // ── Audio session — keep audio alive while locked / in background ────
    if (!_previewMode) {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;
        [session setCategory:AVAudioSessionCategoryPlayAndRecord
                 withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker |
                             AVAudioSessionCategoryOptionAllowBluetoothHFP |
                             AVAudioSessionCategoryOptionMixWithOthers
                       error:&err];
        if (err) NSLog(@"[Parties] Audio session setCategory error: %@", err);
        [session setActive:YES error:&err];
        if (err) NSLog(@"[Parties] Audio session setActive error: %@", err);

        [session requestRecordPermission:^(BOOL granted) {
            if (!granted)
                NSLog(@"[Parties] Microphone permission denied — voice chat will not work.");
        }];
    }

    // ── Metal + RmlUi ────────────────────────────────────────────────────
    _commandQueue = [_view.device newCommandQueue];
    if (!_commandQueue || !Backend::Initialize(_view.device, _view)) {
        NSLog(@"[Parties] Failed to initialize the Metal renderer");
        _view.paused = YES;
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
        _view.paused = YES;
        return;
    }
    _rmlInitialized = true;

    // Register window-action as no-op to suppress warnings (used on Windows for caption hit-testing)
    Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
        .AddParser("keyword", "none, caption, close, minimize, maximize");

    parties::client::register_custom_elements(_elementRegistry);

    _dpRatio = UIScreen.mainScreen.scale;
    CGSize native = UIScreen.mainScreen.nativeBounds.size;
    int physW = (int)native.width;
    int physH = (int)native.height;
    Backend::SetViewport(physW, physH);

    _rmlContext = Rml::CreateContext("main", Rml::Vector2i(physW, physH));
    if (!_rmlContext) {
        NSLog(@"[Parties] Failed to create the RmlUi context");
        _view.paused = YES;
        return;
    }
    _rmlContext->SetDensityIndependentPixelRatio((float)_dpRatio);
    _rmlContext->ActivateTheme("ios", true);

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
    Rml::LoadFontFace("ui/fonts/NotoSans-Regular.ttf", true);
    Rml::LoadFontFace("ui/fonts/NotoSans-Bold.ttf", true);

#ifdef RMLUI_DEBUG
    _debuggerInitialized = Rml::Debugger::Initialise(_rmlContext);
    if (_debuggerInitialized)
        Rml::Debugger::SetVisible(false);
    else
        NSLog(@"[Parties] Failed to initialize the RmlUi debugger");
    UITapGestureRecognizer* dbgTap =
        [[UITapGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(toggleDebugger)];
    dbgTap.numberOfTouchesRequired = 4;
    [_view addGestureRecognizer:dbgTap];
#endif

    // ── Sound player ─────────────────────────────────────────────────────
    if (!_previewMode)
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

    bridge.show_channel_menu = nullptr;
    bridge.show_server_menu = [bself](int server_id) {
        UIAlertController* menu = [UIAlertController
            alertControllerWithTitle:@"Saved Party"
            message:nil
            preferredStyle:UIAlertControllerStyleActionSheet];
        [menu addAction:[UIAlertAction
            actionWithTitle:@"Change Server Nickname"
            style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*) {
                if (bself->_core.server_model_.on_edit_server_nickname)
                    bself->_core.server_model_.on_edit_server_nickname(server_id);
            }]];
        [menu addAction:[UIAlertAction
            actionWithTitle:@"Remove Saved Party"
            style:UIAlertActionStyleDestructive
            handler:^(UIAlertAction*) {
                if (bself->_core.server_model_.on_delete_server)
                    bself->_core.server_model_.on_delete_server(server_id);
            }]];
        [menu addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                style:UIAlertActionStyleCancel
                                              handler:nil]];
        [bself presentViewController:menu animated:YES completion:nil];
    };

    bridge.open_share_picker = nullptr;  // iOS: receive-only, no screen share send

    bridge.on_authenticated = [bself]() {
        bself->_core.net_.open_av_streams();
    };

    bridge.stop_screen_share = nullptr;  // iOS doesn't send screen shares

    bridge.request_keyframe = nullptr;

    // Viewer is a data-for grid of per-sharer cells ("screen-share-<id>"); cells
    // are torn down by the binding when they leave model_.watched.
    bridge.clear_video_element = []() {};

    // iOS manages its single hardware decoder directly in
    // watchSharer:/stopWatching:. The multi-stream bridge callbacks remain
    // unset so AppCore uses its single-select watching path.

#ifndef NDEBUG
    if (_previewMode) {
        // Simulator-only visual harness. It uses the production document,
        // models, UIKit shell and Metal backend, while avoiding network/audio
        // startup so screenshots are deterministic and appear immediately.
        if (!_core.server_model_.init(_rmlContext) ||
            !_core.model_.init(_rmlContext) ||
            !_core.chat_model_.init(_rmlContext)) {
            NSLog(@"[Parties] Failed to initialize iOS UI fixture models");
            return;
        }
        PopulateIOSPreview(_core, _previewScenario);
        NSLog(@"[Parties] Loaded iOS UI fixture: %s", _previewScenario.c_str());
    } else
#endif
    {
        // ── Init QUIC ─────────────────────────────────────────────────────
        if (!parties::quic_init()) {
            NSLog(@"[Parties] MsQuic init failed");
            return;
        }
        _quicInitialized = true;

        // ── Init AppCore ─────────────────────────────────────────────────
        if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
            NSLog(@"[Parties] AppCore init failed");
            return;
        }
        _coreInitialized = true;

        [self installIOSModelCallbacks];
        _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
            [bself onVideoFrameData:sender_id data:data len:len];
        };

        NSString* deviceName = [[UIDevice currentDevice] name];
        _core.load_or_generate_identity(std::string(deviceName.UTF8String));
        _core.load_saved_prefs();
        _core.refresh_server_list();
    }

    // ── UI document ──────────────────────────────────────────────────────
    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->SetClass("platform-ios", true);
        _doc->Show();
        [self updateViewportSize];
    }
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
    // Single hardware decoder: a chip toggle just switches to that single stream.
    _core.model_.on_toggle_watch = [bself](int id) {
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
    // Route to this sharer's grid cell (single-select on iOS).
    std::string elem_id = "screen-share-" + std::to_string(_core.viewing_sharer_.load());
    auto* el = dynamic_cast<VideoElement*>(_doc->GetElementById(elem_id));
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

    // Populate the viewer grid model with this single stream.
    _core.set_single_watched(uid);
    _streamRevealed = false;
    // Don't dirty yet — onVideoDecoded dirties on first frame to avoid black flash
}

- (void)toggleStreamFullscreen
{
    _streamFullscreen = !_streamFullscreen;
    _core.model_.stream_fullscreen = _streamFullscreen;

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

    _core.set_single_watched(0);
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
    int keyboardPx = (int)std::round(_keyboardInsetPt * _dpRatio);
    int physH = (int)(pts.height * _dpRatio) - _viewportTopPx - keyboardPx;
    _viewportHeightPx = (std::max)(1, physH);
    Backend::SetViewport(physW, _viewportHeightPx);
    if (_rmlContext)
        _rmlContext->SetDimensions(Rml::Vector2i(physW, _viewportHeightPx));

    [self applySafeAreaToDocument];
}

- (void)viewSafeAreaInsetsDidChange
{
    [super viewSafeAreaInsetsDidChange];
    [self updateViewportSize];
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
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
    body->SetProperty("padding-bottom", toDp(_keyboardInsetPt > 0.0 ? 8.0 : _safeInsets.bottom));
    body->SetProperty("padding-left",   toDp(_safeInsets.left));
    body->SetProperty("padding-right",  toDp(_safeInsets.right));
}

// ── Keyboard avoidance ───────────────────────────────────────────────────────

- (void)keyboardWillShow:(NSNotification*)note
{
    CGRect kbScreen = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect kb = [self.view convertRect:kbScreen fromView:nil];
    NSTimeInterval dur = [note.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    _keyboardInsetPt = CGRectGetHeight(CGRectIntersection(self.view.bounds, kb));
    [UIView animateWithDuration:dur animations:^{
        [self updateViewportSize];
    }];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_rmlContext) {
            if (Rml::Element* focused = self->_rmlContext->GetFocusElement())
                focused->ScrollIntoView(false);
        }
    });
}

- (void)keyboardWillHide:(NSNotification*)note
{
    NSTimeInterval dur = [note.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    _keyboardInsetPt = 0.0;
    [UIView animateWithDuration:dur animations:^{
        [self updateViewportSize];
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

    if (_coreInitialized) {
        _core.shutdown();
        _coreInitialized = false;
    }

    if (_decoder) {
        _decoder->shutdown();
        _decoder.reset();
    }

#ifdef RMLUI_DEBUG
    if (_debuggerInitialized) {
        Rml::Debugger::Shutdown();
        _debuggerInitialized = false;
    }
#endif
    if (_rmlContext) {
        Rml::RemoveContext(_rmlContext->GetName());
        _rmlContext = nullptr;
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
    if (_quicInitialized) {
        parties::quic_cleanup();
        _quicInitialized = false;
    }
}

#ifdef RMLUI_DEBUG
- (void)toggleDebugger
{
    if (_debuggerInitialized)
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
}
#endif

// ── MTKViewDelegate ──────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    if (!_backendInitialized) return;
    [self updateViewportSize];
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_backendInitialized || !_rmlContext || !_commandQueue) return;

    // Tick shared logic (network, audio levels, FPS counter, etc.)
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

    id<MTLCommandBuffer> cmd = [_commandQueue commandBuffer];

    // Scroll momentum.
    if (_momentumActive) {
        double now = CACurrentMediaTime();
        float  dt  = (float)(now - _lastFrameTime);
        _lastFrameTime = now;
        if (dt > 0.0f && dt < 0.1f) {
            Rml::Element* target = _scrollTarget.get();
            if (!target) {
                _momentumActive = NO;
                _scrollVelocity = 0.0f;
            } else {
                const bool horizontal = _scrollAxis == IOSScrollAxis::Horizontal;
                float before = horizontal ? target->GetScrollLeft() : target->GetScrollTop();
                if (horizontal)
                    target->SetScrollLeft(before + _scrollVelocity * dt);
                else
                    target->SetScrollTop(before + _scrollVelocity * dt);
                float after = horizontal ? target->GetScrollLeft() : target->GetScrollTop();
                if (fabsf(after - before) < 0.01f)
                    _momentumActive = NO;
            }
            _scrollVelocity *= powf(0.998f, dt * 1000.0f);
            if (fabsf(_scrollVelocity) < 5.0f * _dpRatio) {
                _scrollVelocity = 0.0f;
                _momentumActive = NO;
            }
            if (!_momentumActive) {
                _scrollTarget.reset();
                _scrollAxis = IOSScrollAxis::None;
            }
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
            Backend::SetViewport((int)colorTex.width, _viewportHeightPx);
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

- (Rml::Element*)scrollableAncestorOf:(Rml::Element*)element horizontal:(BOOL)horizontal
{
    while (element) {
        const auto& computed = element->GetComputedValues();
        const auto overflow = horizontal ? computed.overflow_x() : computed.overflow_y();
        const bool permitsScroll = overflow == Rml::Style::Overflow::Auto ||
                                   overflow == Rml::Style::Overflow::Scroll;
        const bool hasOverflow = horizontal
            ? element->GetScrollWidth() > element->GetClientWidth() + 0.5f
            : element->GetScrollHeight() > element->GetClientHeight() + 0.5f;
        if (permitsScroll && hasOverflow)
            return element;
        element = element->GetParentNode();
    }
    return nullptr;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_rmlContext || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    _touchStart     = [touch locationInView:_view];
    _touchLast      = _touchStart;
    _isScrolling    = NO;
    _momentumActive = NO;
    _scrollTarget.reset();
    _scrollCandidateX.reset();
    _scrollCandidateY.reset();
    _scrollAxis     = IOSScrollAxis::None;
    _scrollVelocity = 0.0f;
    _lastMoveTime   = CACurrentMediaTime();
    Rml::Vector2f pt = [self physFromPt:_touchStart];
    _rmlContext->ProcessMouseMove((int)pt.x, (int)pt.y, 0);
    // Check if we hit a slider before pressing — if so, lock out scrolling.
    _isDraggingWidget = [self hitTestSlider:pt];
    if (!_isDraggingWidget) {
        Rml::Element* hovered = _rmlContext->GetHoverElement();
        if (Rml::Element* target = [self scrollableAncestorOf:hovered horizontal:YES])
            _scrollCandidateX = target->GetObserverPtr();
        if (Rml::Element* target = [self scrollableAncestorOf:hovered horizontal:NO])
            _scrollCandidateY = target->GetObserverPtr();
    }
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

    float dxPt = (float)(cur.x - _touchLast.x);
    float dyPt = (float)(cur.y - _touchLast.y);
    _touchLast = cur;

    if (!_isScrolling && !_isDraggingWidget) {
        const float totalX = fabsf((float)(cur.x - _touchStart.x));
        const float totalY = fabsf((float)(cur.y - _touchStart.y));
        if ((std::max)(totalX, totalY) > 10.0f) {
            _isScrolling = YES;
            if (totalX > totalY && _scrollCandidateX.get()) {
                _scrollAxis = IOSScrollAxis::Horizontal;
                _scrollTarget = _scrollCandidateX.get()->GetObserverPtr();
            } else if (_scrollCandidateY.get()) {
                _scrollAxis = IOSScrollAxis::Vertical;
                _scrollTarget = _scrollCandidateY.get()->GetObserverPtr();
            } else if (_scrollCandidateX.get()) {
                _scrollAxis = IOSScrollAxis::Horizontal;
                _scrollTarget = _scrollCandidateX.get()->GetObserverPtr();
            }
        }
    }

    Rml::Vector2f pt = [self physFromPt:cur];
    _rmlContext->ProcessMouseMove((int)pt.x, (int)pt.y, 0);

    if (_isScrolling) {
        // Follow the finger one-to-one. Desktop wheel ticks quantize this drag
        // and are the source of the non-native, detached iOS scroll feel.
        const bool horizontal = _scrollAxis == IOSScrollAxis::Horizontal;
        const float deltaPt = horizontal ? dxPt : dyPt;
        if (Rml::Element* target = _scrollTarget.get()) {
            if (horizontal)
                target->SetScrollLeft(target->GetScrollLeft() - deltaPt * _dpRatio);
            else if (_scrollAxis == IOSScrollAxis::Vertical)
                target->SetScrollTop(target->GetScrollTop() - deltaPt * _dpRatio);
        }

        double now = CACurrentMediaTime();
        double dt  = now - _lastMoveTime;
        _lastMoveTime = now;
        if (dt > 0.0 && dt < 0.1) {
            float sample = -deltaPt * _dpRatio / (float)dt;
            _scrollVelocity = 0.72f * _scrollVelocity + 0.28f * sample;
            _scrollVelocity = std::clamp(_scrollVelocity, -12000.0f, 12000.0f);
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

    // A finger held still before release must not resume an old velocity
    // sample. UIPanGestureRecognizer behaves the same way and avoids the
    // surprising glide that desktop-style emulations often produce.
    if (_isScrolling && CACurrentMediaTime() - _lastMoveTime > 0.10)
        _scrollVelocity = 0.0f;

    if (!_isScrolling) {
        if (_isDraggingWidget) {
            // Slider drag — button was pressed in touchesBegan, just release.
            _rmlContext->ProcessMouseButtonUp(0, 0);
        } else {
            // Tap — send down+up together now that we know it's not a scroll.
            _rmlContext->ProcessMouseButtonDown(0, 0);
            _rmlContext->ProcessMouseButtonUp(0, 0);
        }
        _scrollVelocity = 0.0f;
        _scrollTarget.reset();
        _scrollAxis = IOSScrollAxis::None;
    } else if (_scrollTarget.get() && fabsf(_scrollVelocity) > 35.0f * _dpRatio) {
        _momentumActive = YES;
        _lastFrameTime = CACurrentMediaTime();
    } else {
        _scrollTarget.reset();
        _scrollAxis = IOSScrollAxis::None;
    }
    _scrollCandidateX.reset();
    _scrollCandidateY.reset();
    _isScrolling = NO;
    _isDraggingWidget = NO;
    _rmlContext->ProcessMouseLeave();
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    _isScrolling = NO;
    _isDraggingWidget = NO;
    _momentumActive = NO;
    _scrollVelocity = 0.0f;
    _scrollTarget.reset();
    _scrollCandidateX.reset();
    _scrollCandidateY.reset();
    _scrollAxis = IOSScrollAxis::None;
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
    if ([self isInputFocused]) {
        if (@available(iOS 16.0, *)) {
            CGPoint point = [gesture locationInView:_view];
            UIEditMenuConfiguration* config =
                [UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:point];
            [_editMenuInteraction presentEditMenuWithConfiguration:config];
        }
        return;
    }

    // Long-press outside an input maps to a secondary click. This gives touch
    // users the same saved-server context menu as macOS and Windows.
    if (_rmlContext) {
        CGPoint point = [gesture locationInView:_view];
        Rml::Vector2f physical = [self physFromPt:point];
        _rmlContext->ProcessMouseMove((int)physical.x, (int)physical.y, 0);
        _rmlContext->ProcessMouseButtonDown(1, 0);
        _rmlContext->ProcessMouseButtonUp(1, 0);
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
