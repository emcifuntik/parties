#import "PartiesViewController.h"
#import <QuartzCore/QuartzCore.h>   // CACurrentMediaTime(), CADisplayLink
#import <AVFoundation/AVFoundation.h>
#import <Security/Security.h>

#import "VideoDecoderIOS.h"
#import "ScreenShareViewController.h"

#include <unordered_map>

// RmlUi Metal backend
#import "../Backends/RmlUi_Backend_iOS_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
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

// Shared client code
#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>
#include <client/sound_player.h>

// iOS UI data model
#include "app_model.h"

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

// ── Sharer metadata (codec + stream dimensions) ───────────────────────────────

struct SharerInfo {
    VideoCodecId codec  = VideoCodecId::AV1;
    uint32_t     width  = 0;
    uint32_t     height = 0;
};

// ── Keychain helpers ──────────────────────────────────────────────────────────

static NSString* keychainKey(NSString* host, NSString* port, NSString* username) {
    return [NSString stringWithFormat:@"parties_%@_%@_%@", host, port, username];
}

static void keychainSavePassword(NSString* host, NSString* port,
                                  NSString* username, NSString* password)
{
    NSString* key      = keychainKey(host, port, username);
    NSData*   passData = [password dataUsingEncoding:NSUTF8StringEncoding];

    NSDictionary* query = @{
        (__bridge id)kSecClass:       (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrAccount: key,
    };
    NSDictionary* attrs = @{
        (__bridge id)kSecClass:       (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrAccount: key,
        (__bridge id)kSecValueData:   passData,
    };
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                    (__bridge CFDictionaryRef)attrs);
    if (status == errSecItemNotFound)
        SecItemAdd((__bridge CFDictionaryRef)attrs, nil);
}

static NSString* keychainLoadPassword(NSString* host, NSString* port,
                                       NSString* username)
{
    NSString* key = keychainKey(host, port, username);
    NSDictionary* query = @{
        (__bridge id)kSecClass:            (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrAccount:      key,
        (__bridge id)kSecReturnData:       @YES,
        (__bridge id)kSecMatchLimit:       (__bridge id)kSecMatchLimitOne,
    };
    CFDataRef result = nil;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query,
                            (CFTypeRef*)&result) == errSecSuccess && result) {
        NSData* data = (__bridge_transfer NSData*)result;
        return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    }
    return @"";
}

// ── Keyboard host (same as rmlui-iphone) ──────────────────────────────────────

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

// ── PartiesViewController ─────────────────────────────────────────────────────

@interface PartiesViewController () {
    // Metal / RmlUi
    MTKView*                _view;
    id<MTLCommandQueue>     _command_queue;
    Rml::Context*           _context;
    Rml::ElementDocument*   _document;
    CGFloat                 _dp_ratio;

    // Touch / scroll (channels list)
    Rml::Element*           _channels_el;
    CGPoint                 _touch_start;
    CGPoint                 _touch_last;
    BOOL                    _is_scrolling;
    float                   _velocity_y;
    BOOL                    _momentum_active;
    double                  _last_move_time;
    double                  _last_frame_time;

    // Keyboard proxy
    RmlKeyInput*            _key_input;

    // Parties networking
    NetClient*              _net_client;
    AudioEngine*            _audio_engine;
    VoiceMixer*             _voice_mixer;
    SoundPlayer*            _sound_player;

    // App state
    UserId                  _my_user_id;
    ChannelId               _current_channel;
    ChannelKey              _current_channel_key;

    // UI data model
    LobbyModel*             _model;

    // Keepalive timer (30s interval)
    NSTimer*                _keepalive_timer;

    // Safe area padding applied to RmlUi document body
    UIEdgeInsets            _safe_insets;

    // Safe area top in physical pixels — used to offset the Metal viewport so the
    // RmlUi debugger (and all content) renders below the Dynamic Island / status bar.
    int                     _viewport_top_px;

    // Screen share / video
    VideoDecoderIOS*        _video_decoder;
    ScreenShareViewController* _share_vc;
    UserId                  _viewing_sharer_id;
    std::unordered_map<UserId, SharerInfo> _active_sharers;

    // While the screen share viewer is presented, PartiesViewController's own
    // MTKView stops drawing (and therefore stops polling ENet). This display
    // link keeps ENet ticking so video packets continue to arrive.
    CADisplayLink*          _share_enet_link;
}
@end

@implementation PartiesViewController

// ── View setup ────────────────────────────────────────────────────────────────

- (void)loadView
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _view = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:device];
    _view.colorPixelFormat        = MTLPixelFormatBGRA8Unorm;
    _view.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    _view.clearColor              = MTLClearColorMake(0.05, 0.05, 0.08, 1.0);
    _view.clearStencil            = 0;
    _view.delegate                = self;
    _view.preferredFramesPerSecond = 60;
    self.view = _view;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    // Request microphone permission up-front so the user isn't surprised later.
    [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) {
        if (!granted)
            NSLog(@"[Parties] Microphone permission denied — voice chat will not work.");
    }];

    // Initialise Metal backend.
    Backend::Initialize(_view.device, _view);
    _command_queue = [_view.device newCommandQueue];

    // Start RmlUi.
    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());
    Rml::Initialise();

    _dp_ratio = UIScreen.mainScreen.scale;
    CGSize native = UIScreen.mainScreen.nativeBounds.size;
    int phys_w = (int)native.width;
    int phys_h = (int)native.height;
    Backend::SetViewport(phys_w, phys_h);

    _context = Rml::CreateContext("main", Rml::Vector2i(phys_w, phys_h));
    _context->SetDensityIndependentPixelRatio((float)_dp_ratio);

    // Keyboard proxy.
    _key_input = [[RmlKeyInput alloc] initWithFrame:CGRectMake(0, -2, 1, 1)];
    _key_input.rmlContext = _context;
    [_view addSubview:_key_input];

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(keyboardWillShow:)
        name:UIKeyboardWillShowNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(keyboardWillHide:)
        name:UIKeyboardWillHideNotification object:nil];

    // Primary fonts — NotoSans for Cyrillic + full unicode coverage.
    for (NSString* name in @[@"NotoSans-Regular", @"NotoSans-Bold"]) {
        NSString* path = [[NSBundle mainBundle] pathForResource:name ofType:@"ttf"];
        if (path) {
            bool ok = Rml::LoadFontFace(path.UTF8String);
            NSLog(@"[Parties] %@ primary font '%@.ttf'",
                  ok ? @"Loaded" : @"FAILED to load", name);
        } else {
            NSLog(@"[Parties] Primary font not in bundle: %@.ttf (will use fallback)", name);
        }
    }

    // LatoLatin as global fallback_face — RmlUi uses this whenever no loaded
    // family matches the requested font-family, guaranteeing text is always visible.
    NSString* fallbackPath = [[NSBundle mainBundle]
                              pathForResource:@"LatoLatin-Regular" ofType:@"ttf"];
    if (fallbackPath) {
        bool ok = Rml::LoadFontFace(fallbackPath.UTF8String, /*fallback_face=*/true);
        NSLog(@"[Parties] %@ fallback font 'LatoLatin-Regular.ttf'",
              ok ? @"Loaded" : @"FAILED to load");
    } else {
        NSLog(@"[Parties] ERROR: fallback font LatoLatin-Regular.ttf not found in bundle!");
    }

    // Parties networking + audio.
    _net_client   = new NetClient();
    _voice_mixer  = new VoiceMixer();
    _sound_player = new SoundPlayer();
    _audio_engine = new AudioEngine();
    _audio_engine->set_mixer(_voice_mixer);
    _audio_engine->set_sound_player(_sound_player);
    _audio_engine->on_encoded_frame = [self](const uint8_t* data, size_t len) {
        [self sendVoiceFrame:data length:len];
    };
    _audio_engine->init();

    // Data model must be set up BEFORE loading the document so that
    // data-if / data-for bindings evaluate correctly on first render.
    _model = new LobbyModel();
    [self loadSavedServers];
    [self bindModelEvents];
    _model->setup(_context);
    _model->mark_dirty();   // apply initial state (show_login=true, etc.)

    [self loadDocument];

    _channels_el = _document ? _document->GetElementById("channels") : nullptr;

#ifdef RMLUI_DEBUG
    // Visual debugger — toggle with a 4-finger tap anywhere on screen.
    Rml::Debugger::Initialise(_context);
    Rml::Debugger::SetVisible(false); // hidden by default; 4-finger tap to toggle
    UITapGestureRecognizer* dbgTap =
        [[UITapGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(toggleDebugger)];
    dbgTap.numberOfTouchesRequired = 4;
    [_view addGestureRecognizer:dbgTap];
    NSLog(@"[Parties] RmlUi debugger ready — 4-finger tap to show/hide.");
#endif

    // Safe area insets will be applied once the view lays out.
}

- (void)viewSafeAreaInsetsDidChange
{
    [super viewSafeAreaInsetsDidChange];
    _safe_insets = self.view.safeAreaInsets;

    // Compute the safe-area top in physical pixels and shift the entire Metal
    // viewport down by that amount.  This ensures the RmlUi debugger panel and
    // all other context content begin below the Dynamic Island / status bar.
    _viewport_top_px = (int)(_safe_insets.top * _dp_ratio);
    Backend::SetViewportTopOffset(_viewport_top_px);

    // Shrink the context height so it exactly fills the area below the safe-area top.
    CGSize native = UIScreen.mainScreen.nativeBounds.size;
    int phys_w = (int)native.width;
    int phys_h = (int)native.height - _viewport_top_px;
    Backend::SetViewport(phys_w, phys_h);
    if (_context)
        _context->SetDimensions(Rml::Vector2i(phys_w, phys_h));

    [self applySafeAreaToDocument];
}

- (void)applySafeAreaToDocument
{
    if (!_document) return;

    // RmlUi v6 has no GetBody(). Find <body> by iterating document children.
    Rml::Element* body = nullptr;
    for (int i = 0; i < _document->GetNumChildren(); i++) {
        Rml::Element* child = _document->GetChild(i);
        if (child && child->GetTagName() == "body") {
            body = child;
            break;
        }
    }
    if (!body) return;

    auto toDp = [](CGFloat pt) -> Rml::String {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0fdp", (double)pt);
        return buf;
    };

    // padding-top is 0: the Metal viewport already starts below the safe area top,
    // so context y=0 is the first pixel below the Dynamic Island.
    body->SetProperty("padding-top",    "0dp");
    body->SetProperty("padding-bottom", toDp(_safe_insets.bottom));
    body->SetProperty("padding-left",   toDp(_safe_insets.left));
    body->SetProperty("padding-right",  toDp(_safe_insets.right));
}

- (void)loadDocument
{
    NSString* path = [[NSBundle mainBundle] pathForResource:@"lobby" ofType:@"rml"];
    if (path) {
        _document = _context->LoadDocument(path.UTF8String);
        if (_document) {
            _document->Show();
            [self applySafeAreaToDocument];
        }
    } else {
        NSLog(@"[Parties] lobby.rml not found in bundle.");
    }
}

// ── Saved servers (NSUserDefaults + Keychain) ─────────────────────────────────

- (void)loadSavedServers
{
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
    NSArray* list = [ud arrayForKey:@"parties_saved_servers"];
    if (!list) return;

    _model->saved_servers.clear();
    int idx = 0;
    for (NSDictionary* d in list) {
        SavedServer s;
        s.idx      = idx++;
        s.host     = [(NSString*)d[@"host"]     UTF8String] ?: "";
        s.port     = [(NSString*)d[@"port"]     UTF8String] ?: "7800";
        s.username = [(NSString*)d[@"username"] UTF8String] ?: "";
        s.display_name = [NSString stringWithFormat:@"%@:%@",
                          d[@"host"], d[@"port"]].UTF8String;
        // Initials: first 2 chars of host, uppercased
        NSString* host = d[@"host"] ?: @"?";
        s.initials = [[host substringToIndex:MIN(2UL, host.length)] uppercaseString].UTF8String;
        s.is_active = false;
        _model->saved_servers.push_back(s);
    }
}

- (void)persistSavedServers
{
    NSMutableArray* list = [NSMutableArray array];
    for (const auto& s : _model->saved_servers) {
        [list addObject:@{
            @"host":     [NSString stringWithUTF8String:s.host.c_str()],
            @"port":     [NSString stringWithUTF8String:s.port.c_str()],
            @"username": [NSString stringWithUTF8String:s.username.c_str()],
        }];
    }
    [[NSUserDefaults standardUserDefaults] setObject:list forKey:@"parties_saved_servers"];
}

// Returns the index of an existing saved server matching host+port, or -1.
- (int)indexOfSavedServerHost:(NSString*)host port:(NSString*)port
{
    int i = 0;
    for (const auto& s : _model->saved_servers) {
        if (s.host == host.UTF8String && s.port == port.UTF8String) return i;
        i++;
    }
    return -1;
}

// Called after a successful login to upsert the server into the saved list.
- (void)upsertCurrentServerWithHost:(NSString*)host port:(NSString*)port
                           username:(NSString*)username password:(NSString*)password
{
    int existing = [self indexOfSavedServerHost:host port:port];

    SavedServer s;
    s.host         = host.UTF8String;
    s.port         = port.UTF8String;
    s.username     = username.UTF8String;
    s.display_name = [NSString stringWithFormat:@"%@:%@", host, port].UTF8String;
    NSString* initStr = [[host substringToIndex:MIN(2UL, host.length)] uppercaseString];
    s.initials     = initStr.UTF8String;

    if (existing >= 0) {
        s.idx = existing;
        _model->saved_servers[(size_t)existing] = s;
    } else {
        s.idx = (int)_model->saved_servers.size();
        _model->saved_servers.push_back(s);
    }

    // Mark active
    for (auto& sv : _model->saved_servers)
        sv.is_active = (sv.host == s.host && sv.port == s.port);

    keychainSavePassword(host, port, username, password);
    [self persistSavedServers];
    _model->mark_dirty();
}

// ── RmlUi data model event bindings ──────────────────────────────────────────

- (void)bindModelEvents
{
    _model->on_connect = [self]() { [self doConnect]; };

    _model->on_join_channel = [self](uint32_t ch_id) {
        if (!_net_client->is_connected()) return;
        BinaryWriter w;
        w.write_u32(ch_id);
        _net_client->send_message(ControlMessageType::CHANNEL_JOIN,
                                  w.data().data(), w.size());
        _sound_player->play(SoundPlayer::Effect::JoinChannel);
    };

    _model->on_leave_channel = [self]() {
        if (!_net_client->is_connected()) return;
        _net_client->send_message(ControlMessageType::CHANNEL_LEAVE, nullptr, 0);
        _sound_player->play(SoundPlayer::Effect::LeaveChannel);
        _voice_mixer->clear();
        _current_channel = 0;
        _model->current_channel = 0;
        _model->current_channel_name.clear();
        _model->mark_dirty();
    };

    _model->on_toggle_mute = [self]() {
        bool muted = !_audio_engine->is_muted();
        _audio_engine->set_muted(muted);
        _model->is_muted = muted;
        _sound_player->play(muted ? SoundPlayer::Effect::Mute : SoundPlayer::Effect::Unmute);
        _model->mark_dirty();
    };

    _model->on_toggle_deafen = [self]() {
        bool deafened = !_audio_engine->is_deafened();
        _audio_engine->set_deafened(deafened);
        _model->is_deafened = deafened;
        _sound_player->play(deafened ? SoundPlayer::Effect::Deafen : SoundPlayer::Effect::Undeafen);
        _model->mark_dirty();
    };

    _model->on_toggle_settings = [self]() {
        _model->show_settings = !_model->show_settings;
        _model->mark_dirty();
    };

    _model->on_toggle_denoise = [self]() {
        bool enabled = !_audio_engine->is_denoise_enabled();
        _audio_engine->set_denoise_enabled(enabled);
        _model->denoise_enabled = enabled;
        _model->mark_dirty();
    };

    _model->on_register = [self]() { [self doRegister]; };

    _model->on_disconnect = [self]() { [self doDisconnect]; };

    _model->on_select_server = [self](int idx) {
        if (idx < 0 || idx >= (int)_model->saved_servers.size()) return;
        const auto& s = _model->saved_servers[(size_t)idx];

        // Pre-fill login form
        _model->login_host     = s.host;
        _model->login_port     = s.port;
        _model->login_username = s.username;

        NSString* host = [NSString stringWithUTF8String:s.host.c_str()];
        NSString* port = [NSString stringWithUTF8String:s.port.c_str()];
        NSString* user = [NSString stringWithUTF8String:s.username.c_str()];
        NSString* pass = keychainLoadPassword(host, port, user);
        _model->login_password = pass.UTF8String;

        // If already connected, disconnect first
        if (_net_client->is_connected())
            [self doDisconnect];

        // Switch to login screen showing pre-filled fields
        _model->show_login   = true;
        _model->is_connected = false;
        _model->mark_dirty();
    };

    _model->on_add_server = [self]() {
        // Clear form for a new server entry
        _model->login_host.clear();
        _model->login_port     = "7800";
        _model->login_username.clear();
        _model->login_password.clear();
        _model->login_status.clear();
        _model->login_error.clear();
        _model->show_login   = true;
        _model->is_connected = false;
        _model->mark_dirty();
    };

    _model->on_watch_sharer = [self](int uid) {
        [self watchSharer:(UserId)uid];
    };

    _model->on_stop_watching = [self]() {
        [self stopWatching];
    };
}

// ── Connection ────────────────────────────────────────────────────────────────

- (void)doConnect
{
    std::string host = _model->login_host;
    uint16_t    port = (uint16_t)std::stoi(_model->login_port.empty() ? "7800" : _model->login_port);
    std::string user = _model->login_username;
    std::string pass = _model->login_password;

    _model->login_status = "Connecting…";
    _model->login_error.clear();
    _model->mark_dirty();

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        bool ok = _net_client->connect(host, port);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ok) {
                _model->login_status = "Connection failed.";
                _model->login_error  = "Could not reach server.";
                _model->mark_dirty();
                return;
            }
            // Send AUTH_REQUEST
            BinaryWriter w;
            w.write_string(user);
            w.write_string(pass);
            w.write_string("");   // optional server password
            _net_client->send_message(ControlMessageType::AUTH_REQUEST,
                                      w.data().data(), w.size());
            _model->login_status = "Authenticating…";
            _model->mark_dirty();

            // Register ENet data callback.
            _net_client->on_data_received = [self](const uint8_t* data, size_t len) {
                [self onDataPacket:data length:len];
            };
            _net_client->on_disconnected = [self]() {
                dispatch_async(dispatch_get_main_queue(), ^{ [self onDisconnected]; });
            };
        });
    });
}

- (void)doRegister
{
    std::string host = _model->login_host;
    uint16_t    port = (uint16_t)std::stoi(_model->login_port.empty() ? "7800" : _model->login_port);
    std::string user = _model->login_username;
    std::string pass = _model->login_password;

    if (user.empty() || pass.empty()) {
        _model->login_error = "Username and password are required.";
        _model->mark_dirty();
        return;
    }

    _model->login_status = "Connecting for registration…";
    _model->login_error.clear();
    _model->mark_dirty();

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        bool ok = _net_client->connect(host, port);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ok) {
                _model->login_status = "Connection failed.";
                _model->login_error  = "Could not reach server.";
                _model->mark_dirty();
                return;
            }
            BinaryWriter w;
            w.write_string(user);
            w.write_string(pass);
            _net_client->send_message(ControlMessageType::REGISTER_REQUEST,
                                      w.data().data(), w.size());
            _model->login_status = "Registering…";
            _model->mark_dirty();
        });
    });
}

- (void)doDisconnect
{
    // Clear callback first to prevent onDisconnected being called twice
    // if disconnect() also triggers the on_disconnected callback.
    _net_client->on_disconnected = nullptr;
    _net_client->disconnect();
    [self onDisconnected];
}

- (void)onDisconnected
{
    [_keepalive_timer invalidate];
    _keepalive_timer = nil;
    _audio_engine->stop();
    _voice_mixer->clear();
    _current_channel = 0;

    // Stop any active screen share playback.
    _viewing_sharer_id = 0;
    _active_sharers.clear();
    if (_video_decoder) {
        _video_decoder->on_decoded = nullptr;
        _video_decoder->shutdown();
        delete _video_decoder;
        _video_decoder = nullptr;
    }
    if (_share_vc) {
        _share_vc.onDismissed = nil;
        [_share_vc dismissViewControllerAnimated:NO completion:nil];
        _share_vc = nil;
    }

    // Clear active flag on all servers
    for (auto& s : _model->saved_servers)
        s.is_active = false;

    _model->is_connected       = false;
    _model->show_login         = true;
    _model->login_status       = "Disconnected.";
    _model->channels.clear();
    _model->sharers.clear();
    _model->viewing_sharer_id  = 0;
    _model->mark_dirty();
}

// ── Server message dispatch ────────────────────────────────────────────────────

- (void)processServerMessages
{
    while (auto opt = _net_client->incoming().try_pop()) {
        BinaryReader r(opt->payload.data(), opt->payload.size());
        [self handleMessage:opt->type reader:r];
    }
}

- (void)handleMessage:(ControlMessageType)type reader:(BinaryReader&)r
{
    switch (type) {

    case ControlMessageType::AUTH_RESPONSE: {
        _my_user_id = r.read_u32();
        // Server sends session_token first, then enet_token.
        SessionToken session_token;
        r.read_bytes(session_token.data(), 32);
        EnetToken enet_token;
        r.read_bytes(enet_token.data(), 32);
        uint8_t role_byte = r.read_u8();
        std::string server_name = r.read_string();

        std::string host = _model->login_host;
        uint16_t data_port = (uint16_t)(std::stoi(_model->login_port.empty() ? "7800" : _model->login_port) + 1);

        bool enet_ok = _net_client->connect_data(host, data_port, enet_token);
        if (!enet_ok) {
            _model->login_error = "Voice connection failed.";
            _model->mark_dirty();
            return;
        }

        _audio_engine->start();
        _model->is_connected   = true;
        _model->show_login     = false;
        _model->server_name    = server_name;
        _model->username       = _model->login_username;

        // Save server and credentials on successful login
        NSString* host_ns = [NSString stringWithUTF8String:_model->login_host.c_str()];
        NSString* port_ns = [NSString stringWithUTF8String:_model->login_port.c_str()];
        NSString* user_ns = [NSString stringWithUTF8String:_model->login_username.c_str()];
        NSString* pass_ns = [NSString stringWithUTF8String:_model->login_password.c_str()];
        [self upsertCurrentServerWithHost:host_ns port:port_ns
                                 username:user_ns password:pass_ns];

        _model->login_password.clear();
        _model->mark_dirty();

        // Start keepalive.
        _keepalive_timer = [NSTimer scheduledTimerWithTimeInterval:30.0
            target:self selector:@selector(sendKeepalive) userInfo:nil repeats:YES];
        break;
    }

    case ControlMessageType::REGISTER_RESPONSE: {
        uint8_t success = r.read_u8();
        std::string message = r.read_string();
        if (success)
            _model->login_status = "Registered. Please sign in.";
        else
            _model->login_error = message;
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::CHANNEL_LIST: {
        uint32_t count = r.read_u32();
        _model->channels.clear();
        for (uint32_t i = 0; i < count; i++) {
            ChannelInfo ch;
            ch.id         = r.read_u32();
            ch.name       = r.read_string();
            ch.max_users  = r.read_u32();
            ch.sort_order = r.read_u32();
            ch.user_count = r.read_u32();
            _model->channels.push_back(std::move(ch));
        }
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::CHANNEL_USER_LIST: {
        ChannelId ch_id = r.read_u32();
        uint32_t  count = r.read_u32();
        for (auto& ch : _model->channels) {
            if (ch.id == ch_id) {
                ch.users.clear();
                for (uint32_t i = 0; i < count; i++) {
                    ChannelUser u;
                    u.id       = r.read_u32();
                    u.name     = r.read_string();
                    u.role     = (int)r.read_u8();
                    u.muted    = r.read_u8() != 0;
                    u.deafened = r.read_u8() != 0;
                    ch.users.push_back(std::move(u));
                }
                ch.user_count = (uint32_t)ch.users.size();
                break;
            }
        }
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::USER_JOINED_CHANNEL: {
        UserId    uid    = r.read_u32();
        std::string name = r.read_string();
        ChannelId  ch_id = r.read_u32();
        for (auto& ch : _model->channels) {
            if (ch.id == ch_id) {
                ChannelUser u;
                u.id   = uid;
                u.name = name;
                ch.users.push_back(u);
                ch.user_count = (uint32_t)ch.users.size();
                break;
            }
        }
        // Play sound when another user joins the channel we're in.
        if (uid != _my_user_id && ch_id == _current_channel)
            _sound_player->play(SoundPlayer::Effect::UserJoined);
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::USER_LEFT_CHANNEL: {
        UserId    uid   = r.read_u32();
        ChannelId ch_id = r.read_u32();
        for (auto& ch : _model->channels) {
            if (ch.id == ch_id) {
                ch.users.erase(std::remove_if(ch.users.begin(), ch.users.end(),
                    [uid](const ChannelUser& u){ return u.id == uid; }), ch.users.end());
                ch.user_count = (uint32_t)ch.users.size();
                break;
            }
        }
        // Play sound when another user leaves the channel we're in.
        if (uid != _my_user_id && ch_id == _current_channel)
            _sound_player->play(SoundPlayer::Effect::UserLeft);
        _voice_mixer->remove_user(uid);
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::CHANNEL_KEY: {
        ChannelId ch_id = r.read_u32();
        r.read_bytes(_current_channel_key.data(), 32);
        _current_channel = ch_id;
        // Update current_channel_name.
        for (auto& ch : _model->channels) {
            if (ch.id == ch_id) {
                _model->current_channel      = ch_id;
                _model->current_channel_name = ch.name;
                break;
            }
        }
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::SERVER_ERROR: {
        std::string msg_str = r.read_string();
        NSLog(@"[Parties] Server error: %s", msg_str.c_str());
        _model->login_error = msg_str;
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::SCREEN_SHARE_STARTED: {
        UserId   sharer_id = r.read_u32();
        uint8_t  codec_raw = r.read_u8();
        uint16_t width     = r.read_u16();
        uint16_t height    = r.read_u16();

        SharerInfo info;
        info.codec  = (VideoCodecId)codec_raw;
        info.width  = width;
        info.height = height;
        _active_sharers[sharer_id] = info;

        // Find sharer name and flag them as sharing in the channel user list.
        Rml::String sharer_name;
        for (auto& ch : _model->channels) {
            for (auto& u : ch.users) {
                if ((UserId)u.id == sharer_id) {
                    u.is_sharing = true;
                    sharer_name  = u.name;
                }
            }
        }

        // Add to the model's sharers list if not already there.
        bool already_listed = false;
        for (const auto& s : _model->sharers) {
            if ((UserId)s.id == sharer_id) { already_listed = true; break; }
        }
        if (!already_listed) {
            ActiveSharer as;
            as.id   = (int)sharer_id;
            as.name = sharer_name;
            _model->sharers.push_back(as);
        }
        _model->mark_dirty();
        break;
    }

    case ControlMessageType::SCREEN_SHARE_STOPPED: {
        UserId sharer_id = r.read_u32();
        _active_sharers.erase(sharer_id);

        // Clear is_sharing flag.
        for (auto& ch : _model->channels) {
            for (auto& u : ch.users) {
                if ((UserId)u.id == sharer_id) u.is_sharing = false;
            }
        }

        // Remove from sharers list.
        _model->sharers.erase(
            std::remove_if(_model->sharers.begin(), _model->sharers.end(),
                [sharer_id](const ActiveSharer& s){ return (UserId)s.id == sharer_id; }),
            _model->sharers.end());

        // If we were watching this sharer, stop.
        if (_viewing_sharer_id == sharer_id)
            [self stopWatching];

        _model->mark_dirty();
        break;
    }

    case ControlMessageType::KEEPALIVE_PONG:
        break;  // no-op

    default:
        break;
    }
}

// ── ENet data plane ───────────────────────────────────────────────────────────

- (void)onDataPacket:(const uint8_t*)data length:(size_t)len
{
    if (len < 1) return;
    uint8_t ptype = data[0];

    if (ptype == VOICE_PACKET_TYPE && len > 5) {
        // [0x01][sender_id(4)][nonce(12)][tag(16)][ciphertext...]
        UserId sender_id;
        memcpy(&sender_id, data + 1, 4);
        if (sender_id == _my_user_id || _audio_engine->is_deafened())
            return;

        const uint8_t* nonce      = data + 5;
        const uint8_t* tag        = data + 17;
        const uint8_t* ciphertext = data + 33;
        size_t         ct_len     = len - 33;
        if (ct_len == 0) return;

        // AAD = sender_id (4 bytes) — must match what the sender used.
        uint8_t aad[4];
        memcpy(aad, &sender_id, 4);

        std::vector<uint8_t> plaintext(ct_len);
        bool ok = voice_decrypt(_current_channel_key.data(), nonce,
                                aad, 4,
                                ciphertext, ct_len, tag,
                                plaintext.data());
        if (ok)
            _voice_mixer->push_packet(sender_id, plaintext.data(), ct_len);

    } else if (ptype == VIDEO_FRAME_PACKET_TYPE && len >= 33) {
        // [0x02][sender_id(4)][nonce(12)][tag(16)][ciphertext(N)]
        UserId sender_id;
        memcpy(&sender_id, data + 1, 4);
        if (sender_id != _viewing_sharer_id || !_video_decoder) return;

        const uint8_t* nonce      = data + 5;
        const uint8_t* tag        = data + 17;
        const uint8_t* ciphertext = data + 33;
        size_t         ct_len     = len - 33;
        if (ct_len == 0) return;

        uint8_t aad[4];
        memcpy(aad, &sender_id, 4);

        std::vector<uint8_t> plaintext(ct_len);
        if (!voice_decrypt(_current_channel_key.data(), nonce,
                           aad, 4,
                           ciphertext, ct_len, tag,
                           plaintext.data()))
            return;

        // Plaintext: [frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec(1)][data...]
        if (plaintext.size() < 14) return;
        bool is_keyframe = (plaintext[8] & VIDEO_FLAG_KEYFRAME) != 0;
        const uint8_t* encoded     = plaintext.data() + 14;
        size_t         encoded_len = plaintext.size() - 14;
        if (encoded_len == 0) return;

        _video_decoder->decode(encoded, encoded_len, is_keyframe);
    }
}

- (void)sendVoiceFrame:(const uint8_t*)opus_data length:(size_t)len
{
    if (!_net_client->is_connected() || _current_channel == 0 || _audio_engine->is_muted())
        return;

    // Nonce: [user_id(4)][counter(8)]  — user_id embedded so server can identify sender.
    // AAD:   user_id (4 bytes), must match what the receiver uses.
    // Wire:  [type(1)][nonce(12)][tag(16)][ciphertext]
    static uint64_t nonce_counter = 0;
    uint8_t nonce[12] = {};
    memcpy(nonce,     &_my_user_id,   4);
    memcpy(nonce + 4, &nonce_counter, 8);
    nonce_counter++;

    uint8_t aad[4];
    memcpy(aad, &_my_user_id, 4);

    std::vector<uint8_t> ciphertext(len);
    uint8_t tag[16];
    if (!voice_encrypt(_current_channel_key.data(), nonce, aad, 4,
                       opus_data, len, ciphertext.data(), tag))
        return;

    std::vector<uint8_t> packet(1 + 12 + 16 + len);
    packet[0] = VOICE_PACKET_TYPE;
    memcpy(packet.data() + 1,  nonce, 12);
    memcpy(packet.data() + 13, tag, 16);
    memcpy(packet.data() + 29, ciphertext.data(), len);
    _net_client->send_data(packet.data(), packet.size(), /*reliable=*/false);
}

- (void)sendKeepalive
{
    if (_net_client->is_connected())
        _net_client->send_message(ControlMessageType::KEEPALIVE_PING, nullptr, 0);
}

// ── Screen share playback ─────────────────────────────────────────────────────

- (void)watchSharer:(UserId)uid
{
    NSLog(@"[Parties] watchSharer: uid=%u active_sharers.size=%zu viewing=%u",
          (unsigned)uid, _active_sharers.size(), (unsigned)_viewing_sharer_id);

    auto it = _active_sharers.find(uid);
    if (it == _active_sharers.end()) {
        NSLog(@"[Parties] watchSharer: uid=%u NOT found in _active_sharers — "
              "SCREEN_SHARE_STARTED may not have been received", (unsigned)uid);
        return;
    }
    if (uid == _viewing_sharer_id) {
        NSLog(@"[Parties] watchSharer: already watching uid=%u", (unsigned)uid);
        return;
    }

    // Stop previous stream if any.
    if (_viewing_sharer_id != 0) [self stopWatching];

    const SharerInfo& info = it->second;

    // Pre-check codec support so we get a clear error instead of a silent failure.
    if (info.codec == VideoCodecId::AV1 &&
        !VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1)) {
        NSLog(@"[Parties] AV1 hardware decode not available on this device (requires A14 / iPhone 12+)");
        UIAlertController* alert = [UIAlertController
            alertControllerWithTitle:@"Video Unavailable"
            message:@"The sharer is using AV1 video, which requires iPhone 12 or newer. "
                     @"Ask the sharer to switch to H264."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                          style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    // Initialise decoder for this sharer's codec and stream dimensions.
    delete _video_decoder;
    _video_decoder = new VideoDecoderIOS();
    if (!_video_decoder->init(info.codec, info.width, info.height)) {
        NSLog(@"[Parties] VideoDecoderIOS init failed for codec %d", (int)info.codec);
        delete _video_decoder; _video_decoder = nullptr;
        UIAlertController* alert = [UIAlertController
            alertControllerWithTitle:@"Video Error"
            message:@"Could not start the video decoder. The codec may not be "
                     @"supported on this device."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                          style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    _viewing_sharer_id = uid;

    // Subscribe at the server.
    BinaryWriter w;
    w.write_u32((uint32_t)uid);
    _net_client->send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                              w.data().data(), w.size());

    // Request immediate keyframe.
    [self sendPliToSharer:uid];

    // Present fullscreen video view.
    _share_vc = [[ScreenShareViewController alloc] init];
    // Capturing self strongly is intentional: stopWatching sets onDismissed=nil
    // before dismissing, explicitly breaking this cycle.
    _share_vc.onDismissed = ^{ [self stopWatching]; };

    // Pass decoded frames to the screen share view.
    ScreenShareViewController* shareVC = _share_vc;
    _video_decoder->on_decoded = [shareVC](CVPixelBufferRef buf) {
        [shareVC setPixelBuffer:buf];
        CFRelease(buf);
    };

    // Keep ENet ticking while our MTKView is covered by the screen share VC.
    _share_enet_link = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(_pollEnetForShare)];
    [_share_enet_link addToRunLoop:[NSRunLoop mainRunLoop]
                           forMode:NSRunLoopCommonModes];

    _share_vc.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:_share_vc animated:YES completion:nil];

    _model->viewing_sharer_id = (int)uid;
    _model->mark_dirty();
}

- (void)_pollEnetForShare
{
    if (_net_client && _net_client->is_connected()) {
        _net_client->service_enet(0);
        [self processServerMessages];
    }
}

- (void)stopWatching
{
    [_share_enet_link invalidate];
    _share_enet_link = nil;

    if (_viewing_sharer_id == 0) return;

    // Unsubscribe from server.
    BinaryWriter w;
    w.write_u32(0u);
    if (_net_client->is_connected())
        _net_client->send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                                  w.data().data(), w.size());

    UserId was_watching = _viewing_sharer_id;
    _viewing_sharer_id  = 0;

    if (_video_decoder) {
        _video_decoder->on_decoded = nullptr;
        _video_decoder->flush();
        _video_decoder->shutdown();
        delete _video_decoder;
        _video_decoder = nullptr;
    }

    if (_share_vc) {
        _share_vc.onDismissed = nil;
        [_share_vc dismissViewControllerAnimated:YES completion:nil];
        _share_vc = nil;
    }

    _model->viewing_sharer_id = 0;
    _model->mark_dirty();
    (void)was_watching;
}

- (void)sendPliToSharer:(UserId)uid
{
    // [VIDEO_CONTROL_TYPE(1)][VIDEO_CTL_PLI(1)][target_user_id(4)]
    uint8_t pkt[6];
    pkt[0] = VIDEO_CONTROL_TYPE;
    pkt[1] = VIDEO_CTL_PLI;
    memcpy(pkt + 2, &uid, 4);
    _net_client->send_video(pkt, sizeof(pkt), /*reliable=*/true);
}

// ── Keyboard avoidance (same as rmlui-iphone) ─────────────────────────────────

- (void)keyboardWillShow:(NSNotification*)note
{
    CGRect kb     = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect screen = UIScreen.mainScreen.bounds;
    NSTimeInterval dur = [note.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];

    CGFloat shift_y = 0;
    if (_context) {
        Rml::Element* focused = _context->GetFocusElement();
        if (focused) {
            float bottom_pt = (focused->GetAbsoluteTop() + focused->GetClientHeight())
                              / (float)_dp_ratio;
            if (bottom_pt > screen.size.height / 2.0f)
                shift_y = -kb.size.height;
        }
    }
    [UIView animateWithDuration:dur animations:^{
        self->_view.frame = CGRectMake(0, shift_y, screen.size.width, screen.size.height);
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

// ── Orientation ───────────────────────────────────────────────────────────────

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskPortrait;
}

// ── Teardown ──────────────────────────────────────────────────────────────────

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    // When we present a modal VC (e.g. ScreenShareViewController), iOS calls
    // viewDidDisappear on us. Skip teardown in that case — only tear down when
    // the VC is truly being removed (no modal child on screen).
    if (self.presentedViewController) return;

    [_keepalive_timer invalidate];

    if (_audio_engine) { _audio_engine->stop(); _audio_engine->shutdown(); }
    if (_net_client)   { _net_client->disconnect(); }

    if (_video_decoder) {
        _video_decoder->shutdown();
        delete _video_decoder; _video_decoder = nullptr;
    }

    delete _model;        _model        = nullptr;
    delete _audio_engine; _audio_engine = nullptr;
    delete _voice_mixer;  _voice_mixer  = nullptr;
    delete _net_client;   _net_client   = nullptr;

    if (_context) {
        Rml::RemoveContext(_context->GetName());
        _context = nullptr;
    }
#ifdef RMLUI_DEBUG
    Rml::Debugger::Shutdown();
#endif
    Rml::Shutdown();
    Backend::Shutdown();
}

#ifdef RMLUI_DEBUG
- (void)toggleDebugger
{
    Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
}
#endif

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    int w = (int)size.width;
    int h = (int)size.height - _viewport_top_px;
    Backend::SetViewport(w, h);
    if (_context)
        _context->SetDimensions(Rml::Vector2i(w, h));
}

- (void)drawInMTKView:(MTKView*)view
{
    if (!_context) return;

    id<MTLCommandBuffer>      cmd  = [_command_queue commandBuffer];
    MTLRenderPassDescriptor*  pass = view.currentRenderPassDescriptor;
    if (!pass || !cmd) return;

    // Poll ENet for incoming voice packets.
    if (_net_client->is_connected())
        _net_client->service_enet(0);

    // Dispatch server control messages on the main thread.
    [self processServerMessages];

    // Scroll momentum.
    if (_momentum_active && _channels_el) {
        double now = CACurrentMediaTime();
        float  dt  = (float)(now - _last_frame_time);
        _last_frame_time = now;
        if (dt > 0.0f && dt < 0.1f) {
            float delta_px = _velocity_y * dt * (float)_dp_ratio;
            _channels_el->SetScrollTop(_channels_el->GetScrollTop() + delta_px);
            _velocity_y *= powf(0.998f, dt * 1000.0f);
            if (fabsf(_velocity_y) < 30.0f) { _velocity_y = 0.0f; _momentum_active = NO; }
        }
    }

    // Keyboard show/hide based on focused element.
    if (_key_input) {
        Rml::Element* focused = _context->GetFocusElement();
        BOOL want = (focused && focused->GetTagName() == "input");
        if (want  && !_key_input.isFirstResponder) [_key_input becomeFirstResponder];
        if (!want &&  _key_input.isFirstResponder) [_key_input resignFirstResponder];
    }

    // Sync scissor viewport to actual render-pass texture dimensions.
    {
        id<MTLTexture> colorTex = pass.colorAttachments[0].texture;
        if (colorTex)
            Backend::SetViewport((int)colorTex.width, (int)colorTex.height - _viewport_top_px);
    }

    Backend::BeginFrame(cmd, pass);
    _context->Update();
    _context->Render();
    Backend::EndFrame();

    [cmd presentDrawable:view.currentDrawable];
    [cmd commit];
}

// ── Touch input (same pattern as rmlui-iphone) ────────────────────────────────

- (Rml::Vector2f)physFromPt:(CGPoint)p
{
    // Subtract the viewport top offset so context y=0 aligns with the pixel
    // just below the Dynamic Island, matching the Metal viewport origin.
    return { (float)(p.x * _dp_ratio),
             (float)(p.y * _dp_ratio) - (float)_viewport_top_px };
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_context || touches.count == 0) return;
    UITouch* touch   = touches.anyObject;
    _touch_start     = [touch locationInView:_view];
    _touch_last      = _touch_start;
    _is_scrolling    = NO;
    _momentum_active = NO;
    _velocity_y      = 0.0f;
    _last_move_time  = CACurrentMediaTime();
    Rml::Vector2f pt = [self physFromPt:_touch_start];
    _context->ProcessMouseMove((int)pt.x, (int)pt.y, 0);
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_context || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    CGPoint  cur   = [touch locationInView:_view];

    float dx_pt = (float)(cur.x - _touch_last.x);
    float dy_pt = (float)(cur.y - _touch_last.y);
    _touch_last = cur;

    if (!_is_scrolling) {
        float dx = (float)(cur.x - _touch_start.x);
        float dy = (float)(cur.y - _touch_start.y);
        if (dx*dx + dy*dy > 10.0f * 10.0f) _is_scrolling = YES;
    }

    Rml::Vector2f pt = [self physFromPt:cur];
    _context->ProcessMouseMove((int)pt.x, (int)pt.y, 0);

    if (_is_scrolling && _channels_el) {
        _channels_el->SetScrollTop(_channels_el->GetScrollTop() - dy_pt * (float)_dp_ratio);

        double now = CACurrentMediaTime();
        double dt  = now - _last_move_time;
        _last_move_time = now;
        if (dt > 0.0 && dt < 0.1) {
            float sample = -dy_pt / (float)dt;
            _velocity_y  = 0.6f * _velocity_y + 0.4f * sample;
            static const float kMaxVelocity = 1500.0f;
            _velocity_y = std::clamp(_velocity_y, -kMaxVelocity, kMaxVelocity);
        }
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    if (!_context || touches.count == 0) return;
    UITouch* touch = touches.anyObject;
    CGPoint  cur   = [touch locationInView:_view];

    Rml::Vector2f pt = [self physFromPt:cur];
    _context->ProcessMouseMove((int)pt.x, (int)pt.y, 0);

    if (!_is_scrolling) {
        Rml::Vector2f spt = [self physFromPt:_touch_start];
        _context->ProcessMouseMove((int)spt.x, (int)spt.y, 0);
        _context->ProcessMouseButtonDown(0, 0);
        _context->ProcessMouseButtonUp(0, 0);
        _velocity_y = 0.0f;
    } else if (fabsf(_velocity_y) > 50.0f) {
        _momentum_active = YES;
        _last_frame_time = CACurrentMediaTime();
    }
    _is_scrolling = NO;
    _context->ProcessMouseLeave();
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    _is_scrolling = NO;
    if (_context) _context->ProcessMouseLeave();
}

@end
