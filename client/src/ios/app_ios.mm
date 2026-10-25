#import "AppDelegate.h"

#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>

#import "RmlUi_Backend_iOS_Metal.h"
#import "StreamPictureInPicture.h"

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

#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/audio_common.h>
#include <parties/video_common.h>
#include <parties/quic_common.h>

#include <client/app_core.h>
#include <client/sound_player.h>
#include <client/rmlui_backend.h>
#include <client/video_element.h>
#include <client/video_frame_router.h>
#include <client/gradient_circle_element.h>
#include <client/custom_elements.h>
#include <client/ios_audio_routes.h>
#include <client/ios_ui_layout.h>
#include <client/ios_back_gesture.h>
#ifndef NDEBUG
#include <client/ui_fixture.h>
#include <client/ui_layout_audit.h>
#endif

#include <encdec/apple/VideoDecoderIOS.h>

#include <memory>
#include <cstring>

using namespace parties;
using namespace parties::client;

static IOSAudioPort AudioPortSnapshot(AVAudioSessionPortDescription* port)
{
    if (!port) return {};
    const bool paired = [port.portType isEqualToString:AVAudioSessionPortBluetoothHFP] ||
                        [port.portType isEqualToString:AVAudioSessionPortBluetoothLE] ||
                        [port.portType isEqualToString:AVAudioSessionPortHeadsetMic] ||
                        [port.portType isEqualToString:AVAudioSessionPortHeadphones];
    return {port.UID.UTF8String ?: "", port.portName.UTF8String ?: "Audio device", paired,
            static_cast<bool>([port.portType isEqualToString:AVAudioSessionPortBuiltInSpeaker])};
}
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
        "launcher", "sidebar", "island-idle", "island-long-name", "party-modal", "onboarding", "onboarding-restore",
        "onboarding-key-import", "recovery", "room", "chat", "settings",
        "settings-screen-share", "settings-hotkeys", "settings-account",
        "stream-single", "stream-fullscreen", "streams", "member", "login", "tofu",
        "create-channel", "create-text-channel", "rename-channel",
        "global-name", "server-nickname", "launcher-reconnecting", "room-empty",
        "chat-search", "chat-draft", "chat-pinned", "chat-attachment", "settings-select-open",
        "settings-account-import", "login-existing", "settings-airpods", "chat-keyboard", "chat-back-blocked", "room-back-blocked"
    };
    for (const char* value : scenarios)
        if (scenario == value) return true;
    return false;
}

void PopulateIOSPreview(AppCore& core, const std::string& scenario)
{
    if (scenario == "island-idle" || scenario == "island-long-name") {
        PopulateUIFixture(core, scenario, false);
        core.model_.mobile_show_content = false;
        return;
    }
    if (scenario == "launcher-reconnecting" || scenario == "room-empty" ||
        scenario == "chat-search" || scenario == "chat-draft" || scenario == "chat-pinned" || scenario == "chat-attachment" ||
        scenario == "settings-select-open" || scenario == "settings-account-import" || scenario == "login-existing") {
        PopulateUIFixture(core, scenario, false);
        return;
    }
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
    else if (scenario == "stream-single" || scenario == "stream-fullscreen" || scenario == "streams") {
        lobby.router.go(DocumentRoute::Streams);
        lobby.someone_sharing = true;
        lobby.watching_count = scenario == "streams" ? 2 : 1;
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
        lobby.show_import_identity = false;
    }
}
#endif

}

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

#import "PartiesViewController.h"

@implementation PartiesViewController {
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
#ifndef NDEBUG
    unsigned                _previewFrameCount;
#endif

    // Embedded file interface (must outlive RmlUi)
    EmbeddedFileInterface   _fileInterface;

    parties::rml::ElementRegistry _elementRegistry;

    UIScreenEdgePanGestureRecognizer* _backGesture;
    bool _backPending;

    Rml::ObserverPtr<Rml::Element> _scrollTarget;
    Rml::ObserverPtr<Rml::Element> _scrollCandidateX;
    Rml::ObserverPtr<Rml::Element> _scrollCandidateY;
    IOSScrollAxis           _scrollAxis;
    CGPoint                 _touchStart;
    CGPoint                 _touchLast;
    BOOL                    _isScrolling;
    BOOL                    _isDraggingWidget;
    float                   _scrollVelocity;
    BOOL                    _momentumActive;
    double                  _lastMoveTime;
    double                  _lastFrameTime;

    RmlKeyInput*            _keyInput;

    UIEditMenuInteraction*  _editMenuInteraction API_AVAILABLE(ios(16.0));

    UIEdgeInsets            _safeInsets;
    int                     _viewportTopPx;
    int                     _viewportHeightPx;
    CGFloat                 _keyboardInsetPt;

    AppCore                 _core;
    SoundPlayer             _soundPlayer;
    IOSAudioRoutes          _iosAudioRoutes;

    std::unique_ptr<VideoDecoderIOS> _decoder;
    bool                    _streamRevealed;
    PartiesStreamPictureInPicture* _streamPip;
    NSTimer*                _backgroundTick;
    bool                    _streamAudioSession;
    uint32_t                _streamWidth;
    uint32_t                _streamHeight;
    bool                    _streamFullscreen;

    uint32_t _fpsFrameCount;
    std::chrono::steady_clock::time_point _fpsLastUpdate;
}

- (void)loadView
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _view = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:device];
    _view.colorPixelFormat        = MTLPixelFormatBGRA8Unorm;
    _view.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    _view.clearColor              = MTLClearColorMake(10.0 / 255.0, 12.0 / 255.0, 17.0 / 255.0, 1.0);
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

    if (!_previewMode) {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;
        [session setCategory:AVAudioSessionCategoryPlayAndRecord
                 withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker |
                             AVAudioSessionCategoryOptionAllowBluetoothHFP |
                             AVAudioSessionCategoryOptionAllowBluetoothA2DP |
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

    _keyInput = [[RmlKeyInput alloc] initWithFrame:CGRectMake(0, -2, 1, 1)];
    _keyInput.rmlContext = _rmlContext;
    [_view addSubview:_keyInput];

    _backGesture = [[UIScreenEdgePanGestureRecognizer alloc]
        initWithTarget:self action:@selector(handlePageBackGesture:)];
    _backGesture.edges = UIRectEdgeLeft;
    _backGesture.maximumNumberOfTouches = 1;
    _backGesture.delegate = self;
    _backGesture.delaysTouchesBegan = YES;
    _backGesture.cancelsTouchesInView = YES;
    [_view addGestureRecognizer:_backGesture];

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

    if (!_previewMode)
        _soundPlayer.init();

    NSString* docs = [NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString* dbPath = [docs stringByAppendingPathComponent:@"parties.db"];

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

    bridge.open_share_picker = nullptr;

    if (!_previewMode && [PartiesStreamPictureInPicture isSupported]) {
        _streamPip = [[PartiesStreamPictureInPicture alloc] initWithHostView:self.view];
        [_streamPip setOnStarted:[bself]() {
            const UserId stream = bself->_core.viewing_sharer_.load();
            if (stream != 0 && bself->_core.pip_.stream() != stream)
                bself->_core.open_pip(stream);
        }];
        [_streamPip setOnStopped:[bself]() {
            if (bself->_core.pip_.is_open())
                bself->_core.close_pip(PipCloseReason::UserClosed);
        }];
        [_streamPip setOnRestore:[bself](void (^completion)(BOOL)) {
            bself->_core.return_from_pip();
            bself->_rmlContext->Update();
            [bself updateStreamSurface];
            completion(YES);
        }];
        bridge.show_pip = [bself](UserId, const std::string&) {
            [bself exitStreamFullscreen];
            [bself->_streamPip start];
        };
        bridge.hide_pip = [bself](UserId, PipCloseReason reason) {
            [bself->_streamPip stop];
            if (reason == PipCloseReason::StreamUnwatched) {
                [bself->_streamPip flush];
                [bself->_streamPip hideInline];
            }
        };
    }

    bridge.on_authenticated = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{ [bself refreshIOSAudioRoutes]; });
    };

    bridge.stop_screen_share = nullptr;

    bridge.request_keyframe = nullptr;

    bridge.stream_awaiting_keyframe = [bself](UserId id) {
        return bself->_core.viewing_sharer_ == id && bself->_core.awaiting_keyframe_;
    };

    bridge.clear_video_element = []() {};

    bridge.set_keep_awake = [](bool keep_awake) {
        [UIApplication sharedApplication].idleTimerDisabled = keep_awake ? YES : NO;
    };

#ifndef NDEBUG
    if (_previewMode) {
        if (!_core.server_model_.init(_rmlContext) ||
            !_core.model_.init(_rmlContext) ||
            !_core.chat_model_.init(_rmlContext)) {
            NSLog(@"[Parties] Failed to initialize iOS UI fixture models");
            return;
        }
        PopulateIOSPreview(_core, _previewScenario == "settings-airpods" ? "settings" :
            (_previewScenario == "chat-keyboard" || _previewScenario == "chat-back-blocked") ? "chat" :
            _previewScenario == "room-back-blocked" ? "room" : _previewScenario);
        if (_previewScenario == "chat-back-blocked" || _previewScenario == "room-back-blocked") {
            _core.model_.show_rename_channel = true;
            _core.model_.new_rename_channel_name = "general";
        }
        _core.model_.on_stop_watching = [bself]() { bself->_core.set_single_watched(0); };
        _core.model_.on_join_channel = [bself](int id) {
            bself->_core.model_.current_channel = id;
            bself->_core.model_.router.go(DocumentRoute::Room);
        };
        const IOSAudioPort mic{"builtin-mic", "iPhone Microphone"};
        const IOSAudioPort headset{"airpods-hfp", "AirPods Pro", true};
        const bool use_headset = _previewScenario == "settings-airpods";
        _iosAudioRoutes = BuildIOSAudioRoutes({mic, headset}, use_headset ? headset : mic,
            use_headset ? headset : IOSAudioPort{"builtin-speaker", "iPhone Speaker", false, true});
        [self publishIOSAudioRoutes];
        NSLog(@"[Parties] Loaded iOS UI fixture: %s", _previewScenario.c_str());
    } else
#endif
    {
        if (!parties::quic_init()) {
            NSLog(@"[Parties] MsQuic init failed");
            return;
        }
        _quicInitialized = true;

        if (!_core.init(std::string(dbPath.UTF8String), std::move(bridge), _rmlContext)) {
            NSLog(@"[Parties] AppCore init failed");
            return;
        }
        _coreInitialized = true;

        [self installIOSModelCallbacks];
        _core.on_video_frame_received = [bself](uint32_t sender_id, const uint8_t* data, size_t len) {
            [bself onVideoFrameData:sender_id data:data len:len];
        };
        _backgroundTick = [[NSTimer timerWithTimeInterval:1.0 / 60.0
                                                   target:self
                                                 selector:@selector(backgroundTick:)
                                                 userInfo:nil
                                                  repeats:YES] retain];
        [[NSRunLoop mainRunLoop] addTimer:_backgroundTick forMode:NSRunLoopCommonModes];

        NSString* deviceName = [[UIDevice currentDevice] name];
        _core.load_or_generate_identity(std::string(deviceName.UTF8String));
        _core.load_saved_prefs();
        [self refreshIOSAudioRoutes];
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(audioRouteDidChange:)
            name:AVAudioSessionRouteChangeNotification object:[AVAudioSession sharedInstance]];
        _core.refresh_server_list();
    }

    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->SetClass("platform-ios", true);
        _doc->Show();
        [self updateViewportSize];
#ifndef NDEBUG
        if (_previewMode) {
            ApplyUIFixtureDocument(_doc, _previewScenario == "stream-fullscreen"
                ? "stream-single" : _previewScenario);
            if (_previewScenario == "stream-single" || _previewScenario == "stream-fullscreen" ||
                _previewScenario == "streams") {
                _streamWidth = 640;
                _streamHeight = 360;
                _core.model_.on_stream_tap_fullscreen = [bself]() {
                    [bself toggleStreamFullscreen];
                };
                if (_previewScenario == "stream-fullscreen")
                    [self toggleStreamFullscreen];
            }
        }
#endif
    }
}

- (void)installIOSModelCallbacks
{
    PartiesViewController* bself = self;

    _core.model_.on_select_capture = [bself](int index) { [bself selectIOSAudioRoute:index input:YES]; };
    _core.model_.on_select_playback = [bself](int index) { [bself selectIOSAudioRoute:index input:NO]; };

    _core.model_.on_toggle_share = nullptr;
    _core.model_.on_start_native_share = nullptr;
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

    _core.model_.on_stream_tap_fullscreen = [bself]() {
        [bself toggleStreamFullscreen];
    };
}

- (void)refreshIOSAudioRoutes
{
    if (!_coreInitialized || _previewMode) return;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    std::vector<IOSAudioPort> inputs;
    for (AVAudioSessionPortDescription* port in session.availableInputs)
        inputs.push_back(AudioPortSnapshot(port));
    _iosAudioRoutes = BuildIOSAudioRoutes(inputs, AudioPortSnapshot(session.currentRoute.inputs.firstObject),
        AudioPortSnapshot(session.currentRoute.outputs.firstObject));
    [self publishIOSAudioRoutes];
}

- (void)publishIOSAudioRoutes
{
    auto& capture = _core.model_.capture_devices.silent();
    auto& playback = _core.model_.playback_devices.silent();
    capture.clear();
    playback.clear();
    for (size_t i = 0; i < _iosAudioRoutes.inputs.size(); ++i)
        capture.push_back({_iosAudioRoutes.inputs[i].name, static_cast<int>(i)});
    for (size_t i = 0; i < _iosAudioRoutes.outputs.size(); ++i)
        playback.push_back({_iosAudioRoutes.outputs[i].name, static_cast<int>(i)});
    _core.model_.capture_devices.notify();
    _core.model_.playback_devices.notify();
    _core.model_.selected_capture = _iosAudioRoutes.selected_input;
    _core.model_.selected_playback = _iosAudioRoutes.selected_output;
}

- (void)audioRouteDidChange:(NSNotification*)notification
{
    dispatch_async(dispatch_get_main_queue(), ^{ [self refreshIOSAudioRoutes]; });
}

- (void)selectIOSAudioRoute:(int)index input:(BOOL)input
{
    const auto& choices = input ? _iosAudioRoutes.inputs : _iosAudioRoutes.outputs;
    if (index < 0 || index >= static_cast<int>(choices.size())) {
        [self refreshIOSAudioRoutes];
        return;
    }
    if (index == (input ? _iosAudioRoutes.selected_input : _iosAudioRoutes.selected_output)) return;
    const auto choice = choices[index];
    if (choice.kind == IOSAudioRouteKind::CurrentOutput) return;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionPortDescription* preferred = nil;
    if (choice.kind == IOSAudioRouteKind::Input) {
        for (AVAudioSessionPortDescription* port in session.availableInputs) {
            if (choice.uid == (port.UID.UTF8String ?: "")) {
                preferred = port;
                break;
            }
        }
        if (!preferred) {
            [self refreshIOSAudioRoutes];
            return;
        }
    }
    NSError* error = nil;
    BOOL changed = [session overrideOutputAudioPort:AVAudioSessionPortOverrideNone error:&error];
    if (changed) changed = [session setPreferredInput:preferred error:&error];
    if (changed && choice.kind == IOSAudioRouteKind::Speaker)
        changed = [session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker error:&error];
    if (!changed) NSLog(@"[Parties] Audio route selection failed: %@", error);
    [self refreshIOSAudioRoutes];
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
    if (!_streamRevealed) {
        _streamRevealed = true;
        _core.model_.dirty("viewing_sharer_id");
    }
    _core.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(buf);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(buf);
    _streamWidth  = w;
    _streamHeight = h;

    if (_streamPip) {
        [_streamPip enqueuePixelBuffer:buf];
        return;
    }

    if (!_doc) return;
    VideoElement* el = find_stream_grid_video(_doc->GetElementById("stream-grid"),
                                              _core.viewing_sharer_.load());
    if (!el) return;

    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    const uint8_t* y_plane  = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
    const uint8_t* uv_plane = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 1);
    uint32_t y_stride  = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
    uint32_t uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(buf, 1);

    el->UpdateNV12Frame(y_plane, y_stride, uv_plane, uv_stride, w, h);

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
}

- (void)watchSharer:(UserId)uid
{
    if (uid != _core.viewing_sharer_.load()) [_streamPip flush];
    _core.viewing_sharer_   = uid;
    _core.awaiting_keyframe_ = true;
    _decoder = std::make_unique<VideoDecoderIOS>();

    _core.send_pli(uid);

    uint32_t id32 = static_cast<uint32_t>(uid);
    _core.net_.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                            (const uint8_t*)&id32, sizeof(id32));

    _core.set_single_watched(uid);
    _streamRevealed = false;
}

- (void)updateStreamSurface
{
    if (!_streamPip) return;
    Rml::Element* grid = _doc ? _doc->GetElementById("stream-grid") : nullptr;
    VideoElement* cell = find_stream_grid_video(grid, _core.viewing_sharer_.load());
    if (!cell || !cell->IsVisible(true)) {
        const bool pip_starting = cell && _core.pip_.is_open() && ![_streamPip isActive];
        if (!pip_starting) [_streamPip hideInline];
        return;
    }
    auto toPoints = [&](Rml::Element* element, Rml::BoxArea area) {
        const Rml::Vector2f offset = element->GetAbsoluteOffset(area);
        const Rml::Vector2f size = element->GetBox().GetSize(area);
        return CGRectMake(offset.x / _dpRatio, (offset.y + _viewportTopPx) / _dpRatio,
                          size.x / _dpRatio, size.y / _dpRatio);
    };
    CGRect exclude = CGRectNull;
    if (Rml::Element* dock = _doc->QuerySelector(".stream-call-dock"); dock && dock->IsVisible(true))
        exclude = toPoints(dock, Rml::BoxArea::Border);
    [_streamPip setInlineFrame:toPoints(cell, Rml::BoxArea::Content) exclude:exclude];
}

// Automatic picture-in-picture requires the app to own playback: a mixable
// session never qualifies.
- (void)syncStreamAudioSession
{
    if (!_streamPip || !_coreInitialized) return;
    const bool watching = _core.model_.watching_count.get() > 0;
    if (watching == _streamAudioSession) return;
    if (!watching) [_streamPip flush];
    AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionDefaultToSpeaker |
                                            AVAudioSessionCategoryOptionAllowBluetoothHFP |
                                            AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    if (!watching) options |= AVAudioSessionCategoryOptionMixWithOthers;
    NSError* error = nil;
    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayAndRecord
                                     withOptions:options
                                           error:&error];
    if (error) {
        NSLog(@"[Parties] Audio session update for streams failed: %@", error);
        return;
    }
    _streamAudioSession = watching;
}

- (void)backgroundTick:(NSTimer*)timer
{
    if (!_coreInitialized ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateBackground)
        return;
    _core.tick();
    [self syncStreamAudioSession];
}

- (void)toggleStreamFullscreen
{
    _streamFullscreen = !_streamFullscreen;
    _core.model_.stream_fullscreen = _streamFullscreen;

    if (_streamFullscreen && _streamWidth > _streamHeight) {
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

- (void)exitStreamFullscreen
{
    if (!_streamFullscreen) return;
    _streamFullscreen = false;
    _core.model_.stream_fullscreen = false;
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    auto scene = self.view.window.windowScene;
    if (scene) {
        auto prefs = [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:UIInterfaceOrientationMaskPortrait];
        [scene requestGeometryUpdateWithPreferences:prefs
            errorHandler:^(NSError* error) {}];
        [prefs release];
    }
}

- (void)stopWatching
{
    [self exitStreamFullscreen];

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

- (void)updateViewportSize
{
    _safeInsets = self.view.safeAreaInsets;
    _viewportTopPx = (int)std::ceil(_safeInsets.top * _dpRatio);
    Backend::SetViewportTopOffset(_viewportTopPx);

    // Use view bounds (respects current orientation) instead of
    // nativeBounds (always portrait).
    CGSize pts = self.view.bounds.size;
    int physW = (int)(pts.width  * _dpRatio);
    _viewportHeightPx = IOSViewportHeight(pts.height, _safeInsets.top,
        _keyboardInsetPt, _dpRatio);
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
    ApplyIOSSafeArea(_doc, _safeInsets.left, _safeInsets.right, _safeInsets.bottom, _keyboardInsetPt);
}

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

- (UIStatusBarStyle)preferredStatusBarStyle
{
    return UIStatusBarStyleLightContent;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    if (_streamFullscreen && _streamWidth > _streamHeight)
        return UIInterfaceOrientationMaskLandscapeRight | UIInterfaceOrientationMaskLandscapeLeft;
    return UIInterfaceOrientationMaskPortrait;
}

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    if (self.presentedViewController) return;
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    [_backgroundTick invalidate];
    [_backgroundTick release];
    _backgroundTick = nil;

    if (_coreInitialized) {
        _core.shutdown();
        _coreInitialized = false;
    }

    if (_decoder) {
        _decoder->shutdown();
        _decoder.reset();
    }
    [_streamPip stop];
    [_streamPip release];
    _streamPip = nil;

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

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    if (!_backendInitialized) return;
    [self updateViewportSize];
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_backendInitialized || !_rmlContext || !_commandQueue) return;

    if (_coreInitialized)
        _core.tick();

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

#ifndef NDEBUG
    if (_previewMode && _previewScenario == "chat-keyboard" && _previewFrameCount == 20) {
        if (auto* composer = _doc->QuerySelector("input.compose-input")) composer->Focus();
    }
#endif

    if (_keyInput) {
        Rml::Element* focused = _rmlContext->GetFocusElement();
        BOOL want = (focused && focused->GetTagName() == "input");
        if (want  && !_keyInput.isFirstResponder) [_keyInput becomeFirstResponder];
        if (!want &&  _keyInput.isFirstResponder) [_keyInput resignFirstResponder];
    }

    {
        id<MTLTexture> colorTex = pass.colorAttachments[0].texture;
        if (colorTex)
            Backend::SetViewport((int)colorTex.width, _viewportHeightPx);
    }

    _rmlContext->Update();
    [self updateStreamSurface];
    [self syncStreamAudioSession];
    if (_doc) {
        const auto canvas = _doc->GetComputedValues().background_color();
        pass.colorAttachments[0].clearColor = MTLClearColorMake(
            canvas.red / 255.0, canvas.green / 255.0, canvas.blue / 255.0, canvas.alpha / 255.0);
    }
    Backend::BeginFrame(cmd, pass);
    _rmlContext->Render();
    Backend::EndFrame();

#ifndef NDEBUG
    if (_previewMode && _doc && UIApplication.sharedApplication.applicationState == UIApplicationStateActive &&
        ++_previewFrameCount == 60) {
        AuditUIControlLayout(_doc, _previewScenario.c_str());
        if (_previewScenario == "chat-keyboard" && _keyboardInsetPt <= _safeInsets.bottom)
            NSLog(@"[UI audit] FAIL chat-keyboard: software keyboard did not appear");
        NSString* scenario = [NSString stringWithUTF8String:_previewScenario.c_str()];
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if (completed.status == MTLCommandBufferStatusCompleted)
                NSLog(@"[Parties] Ready iOS UI fixture: %@", scenario);
            else
                NSLog(@"[Parties] Failed to render iOS UI fixture: %@", scenario);
        }];
    }
#endif
    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];
}

- (BOOL)canSwipeBackFromPage
{
    return FindIOSBackTarget(_doc, _core.model_.is_connected.get(),
        _core.model_.mobile_show_content.get(), _streamFullscreen,
        self.presentedViewController != nil).action != IOSBackAction::None;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer*)recognizer shouldReceiveTouch:(UITouch*)touch
{
    return recognizer != _backGesture || [self canSwipeBackFromPage];
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)recognizer
{
    if (recognizer != _backGesture) return YES;
    CGPoint direction = [_backGesture translationInView:_view];
    if (std::abs(direction.x) + std::abs(direction.y) < 1.0)
        direction = [_backGesture velocityInView:_view];
    return [self canSwipeBackFromPage] && IsIOSBackSwipeDirection(direction.x, direction.y);
}

- (void)handlePageBackGesture:(UIScreenEdgePanGestureRecognizer*)gesture
{
    if (gesture.state == UIGestureRecognizerStateBegan) {
        [self cancelRmlTouchTracking];
    } else if (gesture.state == UIGestureRecognizerStateEnded && [self canSwipeBackFromPage]) {
        CGPoint translation = [gesture translationInView:_view];
        CGPoint velocity = [gesture velocityInView:_view];
        if (!ShouldCompleteIOSBackSwipe(translation.x, translation.y, velocity.x, _view.bounds.size.width))
            return;
        [self completePageBackNavigation];
    }
}

- (void)completePageBackNavigation
{
    const auto target = FindIOSBackTarget(_doc, _core.model_.is_connected.get(),
        _core.model_.mobile_show_content.get(), _streamFullscreen,
        self.presentedViewController != nil);
    if (target.action == IOSBackAction::None) return;
    if (auto* focused = _rmlContext->GetFocusElement()) focused->Blur();
    [_keyInput resignFirstResponder];
    if (@available(iOS 16.0, *)) [_editMenuInteraction dismissMenu];
    if (target.action == IOSBackAction::ExitFullscreen)
        [self toggleStreamFullscreen];
    else
        target.control->DispatchEvent(Rml::EventId::Click, {});
#ifndef NDEBUG
    NSLog(@"[Navigation] iOS swipe back: route=%s content=%d voice=%d connected=%d fullscreen=%d",
        _core.model_.router.current().c_str(), _core.model_.mobile_show_content.get(),
        _core.model_.current_channel.get(), _core.model_.is_connected.get(), _streamFullscreen);
#endif
}

- (Rml::Vector2f)physFromPt:(CGPoint)p
{
    return { (float)(p.x * _dpRatio),
             (float)(p.y * _dpRatio) - (float)_viewportTopPx };
}

- (BOOL)hitTestSlider:(Rml::Vector2f)pt
{
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
    _backPending = event.allTouches.count == 1 && _touchStart.x <= 24.0 && [self canSwipeBackFromPage];
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
    _isDraggingWidget = [self hitTestSlider:pt];
    if (!_isDraggingWidget) {
        Rml::Element* hovered = _rmlContext->GetHoverElement();
        if (Rml::Element* target = [self scrollableAncestorOf:hovered horizontal:YES])
            _scrollCandidateX = target->GetObserverPtr();
        if (Rml::Element* target = [self scrollableAncestorOf:hovered horizontal:NO])
            _scrollCandidateY = target->GetObserverPtr();
    }
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

    // A coalesced input sequence can reach touch-up without a move sample.
    const float totalX = (float)(cur.x - _touchStart.x);
    const float totalY = (float)(cur.y - _touchStart.y);
    if (_backPending && !_isDraggingWidget && !_isScrolling &&
        ShouldCompleteIOSBackSwipe(totalX, totalY, 0.0f, _view.bounds.size.width)) {
        [self cancelRmlTouchTracking];
        [self completePageBackNavigation];
        return;
    }
    _backPending = false;
    if (!_isDraggingWidget && std::max(std::abs(totalX), std::abs(totalY)) > 10.0f)
        _isScrolling = YES;

    if (_isScrolling && CACurrentMediaTime() - _lastMoveTime > 0.10)
        _scrollVelocity = 0.0f;

    if (!_isScrolling) {
        if (_isDraggingWidget) {
            _rmlContext->ProcessMouseButtonUp(0, 0);
        } else {
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
    [self cancelRmlTouchTracking];
}

- (void)cancelRmlTouchTracking
{
    _backPending = false;
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

- (NSArray<UIKeyCommand*>*)keyCommands {
    return @[
        [UIKeyCommand keyCommandWithInput:@"v" modifierFlags:UIKeyModifierCommand action:@selector(paste:)],
        [UIKeyCommand keyCommandWithInput:@"c" modifierFlags:UIKeyModifierCommand action:@selector(copy:)],
        [UIKeyCommand keyCommandWithInput:@"a" modifierFlags:UIKeyModifierCommand action:@selector(selectAll:)],
    ];
}

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

    if (_rmlContext) {
        CGPoint point = [gesture locationInView:_view];
        Rml::Vector2f physical = [self physFromPt:point];
        _rmlContext->ProcessMouseMove((int)physical.x, (int)physical.y, 0);
        _rmlContext->ProcessMouseButtonDown(1, 0);
        _rmlContext->ProcessMouseButtonUp(1, 0);
    }
}

- (UIMenu*)editMenuInteraction:(UIEditMenuInteraction*)interaction
          menuForConfiguration:(UIEditMenuConfiguration*)configuration
          suggestedActions:(NSArray<UIMenuElement*>*)suggestedActions
    API_AVAILABLE(ios(16.0))
{
    return nil;
}

@end
