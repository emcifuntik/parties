// macOS application — entry point, window, render loop, and app logic.
//
// Mirrors the role of PartiesViewController.mm on iOS, using AppKit instead
// of UIKit.  NSWindow + MTKView host the Metal-backed RmlUi context.

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
#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>
#include <client/settings.h>
#include <client/lobby_model.h>
#include <client/server_list_model.h>
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
        Rml::Vector2f(-(float)event.scrollingDeltaX,
                      -(float)event.scrollingDeltaY),
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

    // Parties core objects
    NetClient       _net;
    AudioEngine     _audio;
    VoiceMixer      _mixer;
    Settings        _settings;
    LobbyModel      _lobbyModel;
    ServerListModel _serverModel;

    // Identity
    std::string _seedPhrase;
    SecretKey   _secretKey;
    PublicKey   _publicKey;
    bool        _hasIdentity;

    // Connection state
    bool        _authenticated;
    UserId      _userId;
    std::string _username;
    std::string _serverHost;
    uint16_t    _serverPort;
    int         _connectingServerId;
    ChannelId   _currentChannel;
    ChannelKey  _channelKey;
    bool        _awaitingConnection;
    uint16_t    _voiceSeq;
    int         _myRole;

    // Screen share — sender
    std::unique_ptr<ScreenCaptureMac>  _capturer;
    std::unique_ptr<VideoEncoderMac>   _encoder;
    uint32_t                           _videoFrameNumber;
    bool                               _sharing;
    bool                               _encoderReady;

    // Screen share — receiver
    std::unique_ptr<VideoDecoderIOS>   _decoder;
    UserId                             _viewingSharer;
    bool                               _awaitingKeyframe;
    std::atomic<uint32_t>              _streamFrameCount;
    std::chrono::steady_clock::time_point _streamFpsLastUpdate;

    // Pending TOFU mismatch
    std::string _tofuPendingFingerprint;
    bool        _tofuPending;
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

    // Set file/system/render interfaces BEFORE Rml::Initialise().
    Rml::SetFileInterface(&_fileInterface);
    Rml::SetSystemInterface(Backend::GetSystemInterface());
    Rml::SetRenderInterface(Backend::GetRenderInterface());

    Rml::Initialise();

    // Register custom element instancers AFTER Rml::Initialise() — in RmlUi 6.x the
    // Factory is a ControlledLifetimeResource that only exists after Initialise().
    Rml::Factory::RegisterElementInstancer("video_frame", &_videoInstancer);

    CGSize physical = _metalView.drawableSize;
    float  dpRatio  = (float)[[NSScreen mainScreen] backingScaleFactor];

    Backend::SetViewport((int)physical.width, (int)physical.height);

    // Context at physical pixel dimensions; dp_ratio makes CSS px values
    // render at correct visual size on Retina (1 CSS px = dpRatio device px).
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
    _authenticated      = false;
    _userId             = 0;
    _serverPort         = DEFAULT_PORT;
    _connectingServerId = 0;
    _currentChannel     = 0;
    _awaitingConnection = false;
    _voiceSeq           = 0;
    _hasIdentity        = false;
    _myRole             = 3;
    _videoFrameNumber   = 0;
    _sharing            = false;
    _encoderReady       = false;
    _viewingSharer      = 0;
    _awaitingKeyframe   = false;
    _streamFrameCount.store(0, std::memory_order_relaxed);
    _streamFpsLastUpdate = std::chrono::steady_clock::now();
    _tofuPending        = false;

    // ── Settings (SQLite) ─────────────────────────────────────────────────
    NSString* appSupport = [NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
    NSString* dir = [appSupport stringByAppendingPathComponent:@"Parties"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
    NSString* dbPath = [dir stringByAppendingPathComponent:@"parties.db"];
    if (!_settings.open(dbPath.UTF8String)) {
        NSLog(@"[Parties] Failed to open settings database at %@", dbPath);
    }

    // ── Models ────────────────────────────────────────────────────────────
    // Must init before loadOrGenerateIdentity which calls _serverModel.dirty().
    [self setupModelCallbacks];
    _lobbyModel.init(_rmlContext);
    _serverModel.init(_rmlContext);

    // ── Identity ──────────────────────────────────────────────────────────
    [self loadOrGenerateIdentity];

    // ── Populate server list ───────────────────────────────────────────────
    [self refreshServerList];

    // ── UI document ───────────────────────────────────────────────────────
    _doc = _rmlContext->LoadDocument("ui/lobby.rml");
    if (_doc) {
        _doc->Show();
        // On macOS we use the native title bar — hide the custom RmlUI one.
        if (auto* tb = _doc->GetElementById("titlebar-drag"))
            tb->SetProperty("display", "none");
    }

    // ── Audio engine ──────────────────────────────────────────────────────
    _audio.set_mixer(&_mixer);
    _audio.init();

    // Voice encoded → send as datagram
    // C++ lambdas capture plain pointers (not __block which is ObjC-block-only).
    PartiesViewController* bself = self;
    _audio.on_encoded_frame = [bself](const uint8_t* data, size_t len) {
        if (!bself->_authenticated || !bself->_currentChannel || bself->_audio.is_muted())
            return;
        uint16_t seq = bself->_voiceSeq++;
        std::vector<uint8_t> pkt(1 + 2 + len);
        pkt[0] = VOICE_PACKET_TYPE;
        std::memcpy(pkt.data() + 1, &seq, 2);
        std::memcpy(pkt.data() + 3, data, len);
        bself->_net.send_data(pkt.data(), pkt.size());
    };

    // ── NetClient callbacks ───────────────────────────────────────────────
    _net.on_disconnected = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{
            [bself onDisconnected];
        });
    };

    _net.on_data_received = [bself](const uint8_t* data, size_t len) {
        [bself onDataReceived:data len:len];
    };

    _net.on_resumption_ticket = [bself](const uint8_t* ticket, size_t len) {
        if (!bself->_serverHost.empty()) {
            bself->_settings.save_resumption_ticket(
                bself->_serverHost, bself->_serverPort, ticket, len);
        }
    };

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
    [self tick];

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

// ── Per-frame tick ────────────────────────────────────────────────────────────

- (void)tick
{
    if (_awaitingConnection) {
        if (_net.is_connected()) {
            _awaitingConnection = false;
            [self onConnected];
        } else if (_net.connect_failed()) {
            _awaitingConnection = false;
            [self onConnectFailed];
        }
    }

    while (auto opt = _net.incoming().try_pop())
        [self handleServerMessage:*opt];

    // Update speaking indicators from mixer levels
    if (_authenticated && _currentChannel && !_audio.is_deafened()) {
        auto levels = _mixer.get_user_levels();
        for (auto& ch : _lobbyModel.channels) {
            for (auto& u : ch.users) {
                auto it = levels.find(static_cast<UserId>(u.id));
                bool speaking = (it != levels.end() && it->second > 0.01f);
                if (u.speaking != speaking) {
                    u.speaking = speaking;
                    _lobbyModel.dirty("channels");
                }
            }
        }
        // Update local mic level
        float lvl = _audio.voice_level();
        if (std::abs(lvl - _lobbyModel.voice_level) > 0.01f) {
            _lobbyModel.voice_level = lvl;
            _lobbyModel.dirty("voice_level");
        }
    }

    // Update stream FPS counter (once per second)
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - _streamFpsLastUpdate).count();
    if (elapsed >= 1.0f) {
        uint32_t sc = _streamFrameCount.exchange(0, std::memory_order_relaxed);
        int sfps = static_cast<int>(sc / elapsed);
        if (sfps != _lobbyModel.stream_fps) {
            _lobbyModel.stream_fps = sfps;
            _lobbyModel.dirty("stream_fps");
        }
        _streamFpsLastUpdate = now;
    }
}

// ── Networking ────────────────────────────────────────────────────────────────

- (void)connectToServerId:(int)serverId
{
    auto servers = _settings.get_saved_servers();
    for (auto& s : servers) {
        if (s.id == serverId) {
            _serverHost         = s.host;
            _serverPort         = static_cast<uint16_t>(s.port);
            _connectingServerId = serverId;

            // Try 0-RTT resumption
            auto ticket = _settings.load_resumption_ticket(s.host, s.port);

            if (_net.connect(s.host, static_cast<uint16_t>(s.port),
                             ticket.empty() ? nullptr : ticket.data(),
                             ticket.size())) {
                _awaitingConnection = true;
                _serverModel.show_login   = true;
                _serverModel.login_status = "Connecting…";
                _serverModel.login_error  = "";
                _serverModel.dirty("show_login");
                _serverModel.dirty("login_status");
                _serverModel.dirty("login_error");
            } else {
                _serverModel.login_error = "Failed to start connection";
                _serverModel.dirty("login_error");
            }
            return;
        }
    }
}

- (void)onConnected
{
    // ── TOFU fingerprint check ────────────────────────────────────────────
    std::string fp = _net.get_server_fingerprint();
    if (!fp.empty() && !_serverHost.empty()) {
        auto result = _settings.check_fingerprint(_serverHost, _serverPort, fp);
        if (result == Settings::TofuResult::Mismatch) {
            // Warn user before proceeding
            _tofuPendingFingerprint = fp;
            _tofuPending            = true;
            _serverModel.tofu_fingerprint = Rml::String(fp);
            _serverModel.show_tofu_warning = true;
            _serverModel.dirty("tofu_fingerprint");
            _serverModel.dirty("show_tofu_warning");
            // Don't send auth yet — wait for user decision
            return;
        } else if (result == Settings::TofuResult::Unknown) {
            _settings.trust_fingerprint(_serverHost, _serverPort, fp);
        }
    }
    [self sendAuthIdentity];
}

- (void)onConnectFailed
{
    NSLog(@"[Parties] Connection failed");
    _serverModel.login_error  = "Connection failed";
    _serverModel.login_status = "";
    _serverModel.dirty("login_error");
    _serverModel.dirty("login_status");
}

- (void)onDisconnected
{
    _authenticated  = false;
    _currentChannel = 0;
    _viewingSharer  = 0;
    _sharing        = false;
    _encoderReady   = false;
    _decoder.reset();
    _encoder.reset();
    _capturer.reset();

    _audio.stop();
    _mixer.clear();

    _lobbyModel.is_connected = false;
    _lobbyModel.channels.clear();
    _lobbyModel.current_channel = 0;
    _lobbyModel.current_channel_name.clear();
    _lobbyModel.sharers.clear();
    _lobbyModel.someone_sharing = false;
    _lobbyModel.dirty_all();

    _serverModel.connected_server_id = 0;
    _serverModel.dirty("connected_server_id");

    NSLog(@"[Parties] Disconnected");
}

- (void)sendAuthIdentity
{
    if (!_hasIdentity) return;

    auto now = static_cast<uint64_t>(std::time(nullptr));

    // Build the data to sign: [pubkey(32)][write_string(username)][u64 timestamp]
    BinaryWriter sig_msg;
    sig_msg.write_bytes(_publicKey.data(), _publicKey.size());
    sig_msg.write_string(_username);
    sig_msg.write_u64(now);

    parties::Signature sig{};
    if (!parties::ed25519_sign(sig_msg.data().data(), sig_msg.data().size(),
                               _secretKey, _publicKey, sig)) {
        _serverModel.login_error  = "Failed to sign auth message";
        _serverModel.login_status = "";
        _serverModel.dirty("login_error");
        _serverModel.dirty("login_status");
        return;
    }

    // AUTH_IDENTITY wire format: [pubkey(32)][write_string(username)][u64 timestamp][signature(64)]
    BinaryWriter writer;
    writer.write_bytes(_publicKey.data(), _publicKey.size());
    writer.write_string(_username);
    writer.write_u64(now);
    writer.write_bytes(sig.data(), sig.size());

    _serverModel.login_status = "Authenticating…";
    _serverModel.dirty("login_status");

    _net.send_message(ControlMessageType::AUTH_IDENTITY,
                      writer.data().data(), writer.data().size());
}

// ── Message dispatch ──────────────────────────────────────────────────────────

- (void)handleServerMessage:(const ServerMessage&)msg
{
    switch (msg.type) {
    case ControlMessageType::AUTH_RESPONSE:
        [self onAuthResponse:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::CHANNEL_LIST:
        [self onChannelList:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::CHANNEL_USER_LIST:
        [self onChannelUserList:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::USER_JOINED_CHANNEL:
        [self onUserJoined:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::USER_LEFT_CHANNEL:
        [self onUserLeft:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::USER_VOICE_STATE:
        [self onUserVoiceState:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::USER_ROLE_CHANGED:
        [self onUserRoleChanged:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::CHANNEL_KEY:
        [self onChannelKey:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::SCREEN_SHARE_STARTED:
        [self onScreenShareStarted:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::SCREEN_SHARE_STOPPED:
        [self onScreenShareStopped:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::SCREEN_SHARE_DENIED:
        [self onScreenShareDenied:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::ADMIN_RESULT:
        [self onAdminResult:msg.payload.data() len:msg.payload.size()];
        break;
    case ControlMessageType::SERVER_ERROR:
        [self onServerError:msg.payload.data() len:msg.payload.size()];
        break;
    default:
        break;
    }
}

// ── Data plane (voice + video) ────────────────────────────────────────────────

- (void)onDataReceived:(const uint8_t*)data len:(size_t)len
{
    if (len < 1) return;
    uint8_t type = data[0];

    if (type == VOICE_PACKET_TYPE) {
        // [type(1)][sender_id(4)][seq(2)][opus(N)]
        if (len < 8) return;
        uint32_t sender_id;
        std::memcpy(&sender_id, data + 1, 4);
        uint16_t seq;
        std::memcpy(&seq, data + 5, 2);
        _mixer.push_packet(sender_id, seq, data + 7, len - 7);
    }
    else if (type == VIDEO_FRAME_PACKET_TYPE) {
        // [type(1)][sender_id(4)][fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][data(N)]
        if (len < 19) return;
        uint32_t sender_id;
        std::memcpy(&sender_id, data + 1, 4);

        if (sender_id != _viewingSharer || !_decoder) return;

        // [type(1)][sender_id(4)][fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][data...]
        //  0        1            5      9      13        14    16    18
        uint8_t  flags  = data[13];
        uint16_t w, h;
        std::memcpy(&w, data + 14, 2);
        std::memcpy(&h, data + 16, 2);
        uint8_t codec_id = data[18];

        bool is_keyframe = (flags & VIDEO_FLAG_KEYFRAME) != 0;
        std::fprintf(stderr, "[Video] frame flags=0x%02x kf=%d awaiting=%d codec=%u %ux%u dlen=%zu\n",
                     flags, is_keyframe, _awaitingKeyframe, codec_id, w, h, len - 19);
        if (_awaitingKeyframe && !is_keyframe) return;
        _awaitingKeyframe = false;

        // Lazy decoder init from first frame dimensions
        if (!_decoder->on_decoded) {
            auto codec = static_cast<VideoCodecId>(codec_id);
            std::fprintf(stderr, "[Video] init decoder codec=%u %ux%u\n", codec_id, w, h);
            if (!_decoder->init(codec, w, h)) {
                std::fprintf(stderr, "[Video] Decoder init failed (codec=%u, %ux%u)\n",
                             codec_id, w, h);
                _decoder.reset();
                return;
            }
            PartiesViewController* bself = self;
            _decoder->on_decoded = [bself](CVPixelBufferRef buf) {
                std::fprintf(stderr, "[Video] decoded frame -> display\n");
                dispatch_async(dispatch_get_main_queue(), ^{
                    [bself onVideoDecoded:buf];
                    CFRelease(buf);
                });
            };
        }

        _decoder->decode(data + 19, len - 19, is_keyframe);
    }
}

- (void)onVideoDecoded:(CVPixelBufferRef)buf
{
    _streamFrameCount.fetch_add(1, std::memory_order_relaxed);

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

// ── Server messages ───────────────────────────────────────────────────────────

- (void)onAuthResponse:(const uint8_t*)data len:(size_t)len
{
    std::printf("[Parties] onAuthResponse: %zu bytes payload\n", len);
    BinaryReader reader(data, len);
    _userId = reader.read_u32();

    uint8_t session_token[32];
    reader.read_bytes(session_token, 32);
    _myRole = reader.read_u8();
    std::string server_name = reader.read_string();
    if (reader.error()) {
        std::fprintf(stderr, "[Parties] onAuthResponse: BinaryReader error (payload too short?)\n");
        return;
    }

    _authenticated = true;

    // Open video stream (stream 4) and voice datagram flow now that auth
    // is complete and stream 0 (control) has its ID locked in.
    _net.open_av_streams();

    // Save server entry with TOFU fingerprint and username
    _settings.save_server(server_name, _serverHost, _serverPort,
                          _net.get_server_fingerprint(), _username);
    [self refreshServerList];

    _lobbyModel.is_connected        = true;
    _lobbyModel.server_name         = Rml::String(server_name);
    _lobbyModel.username            = Rml::String(_username);
    _lobbyModel.my_role             = _myRole;
    _lobbyModel.can_manage_channels = (_myRole <= static_cast<int>(parties::Role::Moderator));
    _lobbyModel.can_kick            = (_myRole <= static_cast<int>(parties::Role::Moderator));
    _lobbyModel.can_manage_roles    = (_myRole <= static_cast<int>(parties::Role::Admin));
    _lobbyModel.dirty("is_connected");
    _lobbyModel.dirty("server_name");
    _lobbyModel.dirty("username");
    _lobbyModel.dirty("my_role");
    _lobbyModel.dirty("can_manage_channels");
    _lobbyModel.dirty("can_kick");
    _lobbyModel.dirty("can_manage_roles");

    _serverModel.connected_server_id = _connectingServerId;
    _serverModel.show_login          = false;
    _serverModel.login_error         = "";
    _serverModel.login_status        = "";
    _serverModel.dirty("connected_server_id");
    _serverModel.dirty("show_login");
    _serverModel.dirty("login_error");
    _serverModel.dirty("login_status");

    // Populate audio device lists
    auto caps = _audio.get_capture_devices();
    _lobbyModel.capture_devices.clear();
    for (auto& d : caps) {
        AudioDevice ad;
        ad.name  = Rml::String(d.name);
        ad.index = d.index;
        _lobbyModel.capture_devices.push_back(ad);
    }
    auto plays = _audio.get_playback_devices();
    _lobbyModel.playback_devices.clear();
    for (auto& d : plays) {
        AudioDevice ad;
        ad.name  = Rml::String(d.name);
        ad.index = d.index;
        _lobbyModel.playback_devices.push_back(ad);
    }
    _lobbyModel.dirty("capture_devices");
    _lobbyModel.dirty("playback_devices");
}

- (void)onChannelList:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    std::unordered_map<int, Rml::Vector<ChannelUser>> old_users;
    for (auto& ch : _lobbyModel.channels)
        old_users[ch.id] = std::move(ch.users);

    _lobbyModel.channels.clear();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ch_id    = reader.read_u32();
        std::string name  = reader.read_string();
        uint32_t max_u    = reader.read_u32();
        uint32_t sort_ord = reader.read_u32();
        uint32_t user_cnt = reader.read_u32();
        if (reader.error()) break;
        (void)sort_ord;

        ChannelInfo ch;
        ch.id        = static_cast<int>(ch_id);
        ch.name      = Rml::String(name);
        ch.max_users = static_cast<int>(max_u);
        ch.user_count = static_cast<int>(user_cnt);

        auto it = old_users.find(ch.id);
        if (it != old_users.end()) {
            ch.users      = std::move(it->second);
            ch.user_count = static_cast<int>(ch.users.size());
        }
        _lobbyModel.channels.push_back(std::move(ch));
    }
    _lobbyModel.dirty("channels");
}

- (void)onChannelUserList:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    ChannelId channel_id = reader.read_u32();
    uint32_t count       = reader.read_u32();
    if (reader.error()) return;

    Rml::Vector<ChannelUser> users;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t uid       = reader.read_u32();
        std::string uname  = reader.read_string();
        uint8_t  urole     = reader.read_u8();
        uint8_t  muted     = reader.read_u8();
        uint8_t  deafened  = reader.read_u8();
        if (reader.error()) break;

        ChannelUser u;
        u.id       = static_cast<int>(uid);
        u.name     = Rml::String(uname);
        u.role     = urole;
        u.muted    = muted != 0;
        u.deafened = deafened != 0;
        users.push_back(u);
    }

    for (auto& ch : _lobbyModel.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ch.users      = std::move(users);
            ch.user_count = static_cast<int>(ch.users.size());

            if (static_cast<int>(channel_id) == _lobbyModel.current_channel) {
                _lobbyModel.current_channel_name = ch.name;
                _audio.start();
            }
            break;
        }
    }
    _lobbyModel.dirty("channels");
    _lobbyModel.dirty("current_channel_name");
}

- (void)onUserJoined:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    std::string uname   = reader.read_string();
    uint32_t channel_id = reader.read_u32();
    uint8_t  urole      = reader.has_remaining(1) ? reader.read_u8() : 3;
    if (reader.error()) return;

    for (auto& ch : _lobbyModel.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ChannelUser u;
            u.id   = static_cast<int>(uid);
            u.name = Rml::String(uname);
            u.role = urole;
            ch.users.push_back(u);
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }
    _lobbyModel.dirty("channels");
}

- (void)onUserLeft:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    for (auto& ch : _lobbyModel.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            auto it = std::remove_if(ch.users.begin(), ch.users.end(),
                [uid](const ChannelUser& u) { return u.id == static_cast<int>(uid); });
            ch.users.erase(it, ch.users.end());
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }
    _mixer.remove_user(static_cast<UserId>(uid));
    _lobbyModel.dirty("channels");
}

- (void)onUserVoiceState:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint32_t uid   = reader.read_u32();
    uint8_t  muted = reader.read_u8();
    uint8_t  deaf  = reader.read_u8();
    if (reader.error()) return;

    for (auto& ch : _lobbyModel.channels) {
        for (auto& u : ch.users) {
            if (u.id == static_cast<int>(uid)) {
                u.muted    = muted != 0;
                u.deafened = deaf  != 0;
                _lobbyModel.dirty("channels");
                return;
            }
        }
    }
}

- (void)onUserRoleChanged:(const uint8_t*)data len:(size_t)len
{
    if (len < 5) return;
    uint32_t uid;
    std::memcpy(&uid, data, 4);
    uint8_t new_role = data[4];

    if (uid == _userId) {
        _myRole = new_role;
        _lobbyModel.my_role             = new_role;
        _lobbyModel.can_manage_channels = (_myRole <= static_cast<int>(parties::Role::Moderator));
        _lobbyModel.can_kick            = (_myRole <= static_cast<int>(parties::Role::Moderator));
        _lobbyModel.can_manage_roles    = (_myRole <= static_cast<int>(parties::Role::Admin));
        _lobbyModel.dirty("my_role");
        _lobbyModel.dirty("can_manage_channels");
        _lobbyModel.dirty("can_kick");
        _lobbyModel.dirty("can_manage_roles");
    }
    for (auto& ch : _lobbyModel.channels) {
        for (auto& u : ch.users) {
            if (u.id == static_cast<int>(uid)) {
                u.role = new_role;
                _lobbyModel.dirty("channels");
                break;
            }
        }
    }
}

- (void)onChannelKey:(const uint8_t*)data len:(size_t)len
{
    if (len < _channelKey.size()) return;
    std::memcpy(_channelKey.data(), data, _channelKey.size());
}

- (void)onScreenShareStarted:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint32_t sharer_id = reader.read_u32();
    if (reader.error()) return;

    // Server doesn't send the name — look it up from the channel user list
    std::string sharer_name = "Unknown";
    for (auto& ch : _lobbyModel.channels) {
        if (ch.id == static_cast<int>(_currentChannel)) {
            for (auto& u : ch.users) {
                if (u.id == static_cast<int>(sharer_id)) {
                    sharer_name = u.name.c_str();
                    break;
                }
            }
            break;
        }
    }

    ActiveSharer s;
    s.id   = static_cast<int>(sharer_id);
    s.name = Rml::String(sharer_name);

    // Remove duplicates
    auto it = std::remove_if(_lobbyModel.sharers.begin(), _lobbyModel.sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    _lobbyModel.sharers.erase(it, _lobbyModel.sharers.end());
    _lobbyModel.sharers.push_back(s);
    _lobbyModel.someone_sharing = !_lobbyModel.sharers.empty();
    _lobbyModel.dirty("sharers");
    _lobbyModel.dirty("someone_sharing");
}

- (void)onScreenShareStopped:(const uint8_t*)data len:(size_t)len
{
    if (len < 4) return;
    uint32_t sharer_id;
    std::memcpy(&sharer_id, data, 4);

    auto it = std::remove_if(_lobbyModel.sharers.begin(), _lobbyModel.sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    _lobbyModel.sharers.erase(it, _lobbyModel.sharers.end());
    _lobbyModel.someone_sharing = !_lobbyModel.sharers.empty();
    _lobbyModel.dirty("sharers");
    _lobbyModel.dirty("someone_sharing");

    if (_viewingSharer == static_cast<UserId>(sharer_id)) {
        _viewingSharer = 0;
        _decoder.reset();
        _lobbyModel.viewing_sharer_id = 0;
        _lobbyModel.dirty("viewing_sharer_id");
    }
}

- (void)onScreenShareDenied:(const uint8_t*)data len:(size_t)len
{
    _lobbyModel.is_sharing = false;
    _lobbyModel.dirty("is_sharing");
    _sharing = false;
    NSLog(@"[Parties] Screen share denied by server");
}

- (void)onAdminResult:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    uint8_t ok  = reader.read_u8();
    std::string msg = reader.read_string();
    if (!msg.empty()) {
        _lobbyModel.admin_message = Rml::String(msg);
        _lobbyModel.dirty("admin_message");
    }
    (void)ok;
}

- (void)onServerError:(const uint8_t*)data len:(size_t)len
{
    BinaryReader reader(data, len);
    std::string msg = reader.read_string();
    if (!msg.empty()) {
        _lobbyModel.error_text = Rml::String(msg);
        _lobbyModel.dirty("error_text");
    }
}

// ── Screen share — sender ─────────────────────────────────────────────────────

- (void)showSharePicker
{
    // Show the share picker overlay (macOS uses native picker button)
    _lobbyModel.use_native_picker = true;
    _lobbyModel.show_share_picker = true;
    _lobbyModel.dirty("use_native_picker");
    _lobbyModel.dirty("show_share_picker");
}

- (void)startNativeShare
{
    // Hide overlay and launch native macOS picker
    _lobbyModel.show_share_picker = false;
    _lobbyModel.dirty("show_share_picker");

    _capturer = std::make_unique<ScreenCaptureMac>();
    PartiesViewController* bself = self;

    static const uint32_t fps_table[] = { 15, 30, 60, 120 };
    uint32_t capture_fps = fps_table[std::min(_lobbyModel.share_fps, 3)];

    _capturer->pick_and_start(capture_fps, [bself](bool success) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!success) {
                bself->_capturer.reset();
                return;
            }
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

        // Lazy encoder init on first frame (actual dimensions known here)
        if (!bself->_encoderReady) {
            uint32_t bitrate = (uint32_t)(bself->_lobbyModel.share_bitrate * 1000000.0f);
            static const uint32_t fps_table[] = { 15, 30, 60, 120 };
            uint32_t fps = fps_table[std::min(bself->_lobbyModel.share_fps, 3)];
            if (!bself->_encoder->init(MacVideoCodec::H265, w, h, bitrate, fps)) {
                bself->_encoder.reset();
                return;
            }
            bself->_encoderReady = true;

            bself->_encoder->on_encoded = [bself](const uint8_t* data, size_t len, bool is_kf) {
                if (!bself->_authenticated) return;

                uint32_t fn = bself->_videoFrameNumber++;
                uint32_t ts = 0;
                uint8_t  flags = is_kf ? VIDEO_FLAG_KEYFRAME : 0;
                uint16_t fw = (uint16_t)bself->_capturer->width();
                uint16_t fh = (uint16_t)bself->_capturer->height();
                uint8_t  codec = static_cast<uint8_t>(VideoCodecId::H265);

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
                bself->_net.send_video(pkt.data(), pkt.size(), true);
            };

            // Notify server and update UI
            dispatch_async(dispatch_get_main_queue(), ^{
                bself->_sharing = true;
                bself->_lobbyModel.is_sharing = true;
                bself->_lobbyModel.dirty("is_sharing");
                bself->_net.send_message(ControlMessageType::SCREEN_SHARE_START, nullptr, 0);
            });
        }

        bself->_encoder->encode(buf, false);
    };

    _capturer->on_closed = [bself]() {
        dispatch_async(dispatch_get_main_queue(), ^{
            [bself stopScreenShare];
        });
    };
}

- (void)stopScreenShare
{
    if (_capturer) _capturer->stop();
    _encoder.reset();
    _capturer.reset();
    _sharing      = false;
    _encoderReady = false;
    _videoFrameNumber = 0;

    _lobbyModel.is_sharing = false;
    _lobbyModel.dirty("is_sharing");

    if (_authenticated)
        _net.send_message(ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

// ── Screen share — receiver ───────────────────────────────────────────────────

- (void)watchSharer:(UserId)sharerId
{
    _viewingSharer    = sharerId;
    _awaitingKeyframe = true;
    _decoder          = std::make_unique<VideoDecoderIOS>();
    // on_decoded wired lazily in onDataReceived on first frame

    // Request PLI so we get a keyframe immediately
    [self sendPLI:sharerId];

    uint32_t id32 = static_cast<uint32_t>(sharerId);
    _net.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                      (const uint8_t*)&id32, sizeof(id32));

    _lobbyModel.viewing_sharer_id = static_cast<int>(sharerId);
    _lobbyModel.dirty("viewing_sharer_id");
}

- (void)stopWatching
{
    _viewingSharer = 0;
    _decoder.reset();
    _awaitingKeyframe = false;

    uint32_t zero = 0;
    _net.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                      (const uint8_t*)&zero, sizeof(zero));

    _lobbyModel.viewing_sharer_id = 0;
    _lobbyModel.dirty("viewing_sharer_id");

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
    _net.send_video(pkt.data(), pkt.size(), true);
}

// ── Identity ──────────────────────────────────────────────────────────────────

- (void)loadOrGenerateIdentity
{
    auto identity = _settings.load_identity();
    if (identity) {
        _seedPhrase  = identity->seed_phrase;
        _secretKey   = identity->secret_key;
        _publicKey   = identity->public_key;
        _hasIdentity = true;
        NSLog(@"[Parties] Loaded identity from settings");
    } else {
        [self generateIdentity];
    }

    // Default username from system hostname
    if (_username.empty())
        _username = NSProcessInfo.processInfo.hostName.UTF8String;

    // Reflect identity state in server model
    _serverModel.has_identity = _hasIdentity;
    _serverModel.fingerprint  = Rml::String(_settings.get_fingerprint());
    _serverModel.dirty("has_identity");
    _serverModel.dirty("fingerprint");
}

- (void)generateIdentity
{
    std::string seed = parties::generate_seed_phrase();
    parties::derive_keypair(seed, _secretKey, _publicKey);
    _seedPhrase  = seed;
    _hasIdentity = true;
    _settings.save_identity(seed, _secretKey, _publicKey);
    NSLog(@"[Parties] Generated new identity");
}

// ── Server list ───────────────────────────────────────────────────────────────

- (void)refreshServerList
{
    auto servers = _settings.get_saved_servers();
    _serverModel.servers.clear();
    for (auto& s : servers) {
        ServerEntry e;
        e.id            = s.id;
        e.name          = Rml::String(s.name);
        e.host          = Rml::String(s.host);
        e.port          = s.port;
        e.last_username = Rml::String(s.last_username);
        if (s.name.size() >= 2)
            e.initials = Rml::String(s.name.substr(0, 2));
        else if (s.name.size() == 1)
            e.initials = Rml::String(s.name.substr(0, 1));
        _serverModel.servers.push_back(e);
    }
    _serverModel.dirty("servers");
}

// ── Model callbacks ───────────────────────────────────────────────────────────

- (void)setupModelCallbacks
{
    PartiesViewController* bself = self;

    // ── ServerListModel ───────────────────────────────────────────────────

    _serverModel.on_connect_server = [bself](int server_id) {
        bself->_serverModel.show_login = true;
        bself->_serverModel.login_username = "";
        bself->_serverModel.login_error    = "";
        bself->_serverModel.login_status   = "";
        bself->_serverModel.dirty("show_login");
        bself->_serverModel.dirty("login_username");
        bself->_serverModel.dirty("login_error");
        bself->_serverModel.dirty("login_status");
        bself->_connectingServerId = server_id;

        // Pre-fill last username
        auto servers = bself->_settings.get_saved_servers();
        for (auto& s : servers) {
            if (s.id == server_id && !s.last_username.empty()) {
                bself->_serverModel.login_username = Rml::String(s.last_username);
                bself->_serverModel.dirty("login_username");
            }
        }
    };

    _serverModel.on_do_connect = [bself]() {
        std::string uname = bself->_serverModel.login_username.c_str();
        if (uname.empty()) {
            bself->_serverModel.login_error = "Enter a username";
            bself->_serverModel.dirty("login_error");
            return;
        }
        bself->_username = uname;
        [bself connectToServerId:bself->_connectingServerId];
    };

    _serverModel.on_cancel_login = [bself]() {
        bself->_serverModel.show_login = false;
        bself->_serverModel.dirty("show_login");
        bself->_net.disconnect();
        bself->_awaitingConnection = false;
    };

    _serverModel.on_save_server = [bself]() {
        std::string host = bself->_serverModel.edit_host.c_str();
        std::string port_str = bself->_serverModel.edit_port.c_str();
        if (host.empty()) {
            bself->_serverModel.edit_error = "Enter a host";
            bself->_serverModel.dirty("edit_error");
            return;
        }
        int port = port_str.empty() ? DEFAULT_PORT : std::atoi(port_str.c_str());
        if (port <= 0 || port > 65535) {
            bself->_serverModel.edit_error = "Invalid port";
            bself->_serverModel.dirty("edit_error");
            return;
        }
        bself->_settings.save_server(host, host, port, "", "");
        bself->_serverModel.show_add_form = false;
        bself->_serverModel.edit_host  = "";
        bself->_serverModel.edit_port  = "";
        bself->_serverModel.edit_error = "";
        bself->_serverModel.dirty("show_add_form");
        bself->_serverModel.dirty("edit_host");
        bself->_serverModel.dirty("edit_port");
        bself->_serverModel.dirty("edit_error");
        [bself refreshServerList];
    };

    _serverModel.on_delete_server = [bself](int server_id) {
        bself->_settings.delete_server(server_id);
        [bself refreshServerList];
    };

    _serverModel.on_generate_identity = [bself]() {
        [bself generateIdentity];
        bself->_serverModel.seed_phrase   = Rml::String(bself->_seedPhrase);
        bself->_serverModel.has_identity  = true;
        bself->_serverModel.fingerprint   = Rml::String(bself->_settings.get_fingerprint());
        bself->_serverModel.show_onboarding = false;
        bself->_serverModel.dirty("seed_phrase");
        bself->_serverModel.dirty("has_identity");
        bself->_serverModel.dirty("fingerprint");
        bself->_serverModel.dirty("show_onboarding");
    };

    _serverModel.on_save_identity = [bself]() {
        // User has confirmed they saved their seed phrase — hide onboarding
        bself->_serverModel.show_onboarding = false;
        bself->_serverModel.dirty("show_onboarding");
    };

    _serverModel.on_show_restore = [bself]() {
        bself->_serverModel.show_restore = true;
        bself->_serverModel.restore_phrase = "";
        bself->_serverModel.dirty("show_restore");
        bself->_serverModel.dirty("restore_phrase");
    };

    _serverModel.on_restore_identity = [bself]() {
        // TODO: implement BIP39 seed phrase restore
        bself->_serverModel.show_restore = false;
        bself->_serverModel.dirty("show_restore");
    };

    _serverModel.on_copy_fingerprint = [bself]() {
        NSString* fp = [NSString stringWithUTF8String:bself->_settings.get_fingerprint().c_str()];
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:fp forType:NSPasteboardTypeString];
    };

    _serverModel.on_tofu_accept = [bself]() {
        if (!bself->_tofuPending) return;
        bself->_settings.trust_fingerprint(bself->_serverHost, bself->_serverPort,
                                            bself->_tofuPendingFingerprint);
        bself->_tofuPending = false;
        bself->_serverModel.show_tofu_warning = false;
        bself->_serverModel.dirty("show_tofu_warning");
        [bself sendAuthIdentity];
    };

    _serverModel.on_tofu_reject = [bself]() {
        bself->_tofuPending = false;
        bself->_serverModel.show_tofu_warning = false;
        bself->_serverModel.show_login        = false;
        bself->_serverModel.dirty("show_tofu_warning");
        bself->_serverModel.dirty("show_login");
        bself->_net.disconnect();
        bself->_awaitingConnection = false;
    };

    // ── LobbyModel ────────────────────────────────────────────────────────

    _lobbyModel.on_join_channel = [bself](int channel_id) {
        if (!bself->_authenticated) return;
        uint32_t id32 = static_cast<uint32_t>(channel_id);
        bself->_net.send_message(ControlMessageType::CHANNEL_JOIN,
                                  (const uint8_t*)&id32, sizeof(id32));
        bself->_currentChannel = static_cast<ChannelId>(channel_id);
        bself->_lobbyModel.current_channel = channel_id;
        bself->_lobbyModel.dirty("current_channel");
    };

    _lobbyModel.on_leave_channel = [bself]() {
        if (!bself->_authenticated) return;
        bself->_net.send_message(ControlMessageType::CHANNEL_LEAVE, nullptr, 0);
        bself->_audio.stop();
        bself->_mixer.clear();
        bself->_currentChannel = 0;
        bself->_lobbyModel.current_channel = 0;
        bself->_lobbyModel.current_channel_name.clear();
        bself->_lobbyModel.dirty("current_channel");
        bself->_lobbyModel.dirty("current_channel_name");
    };

    _lobbyModel.on_toggle_mute = [bself]() {
        bool muted = !bself->_audio.is_muted();
        bself->_audio.set_muted(muted);
        bself->_lobbyModel.is_muted = muted;
        bself->_lobbyModel.dirty("is_muted");

        uint8_t payload[2] = { (uint8_t)muted, (uint8_t)bself->_audio.is_deafened() };
        bself->_net.send_message(ControlMessageType::VOICE_STATE_UPDATE, payload, 2);
    };

    _lobbyModel.on_toggle_deafen = [bself]() {
        bool deaf = !bself->_audio.is_deafened();
        bself->_audio.set_deafened(deaf);
        bself->_lobbyModel.is_deafened = deaf;
        bself->_lobbyModel.dirty("is_deafened");

        uint8_t payload[2] = { (uint8_t)bself->_audio.is_muted(), (uint8_t)deaf };
        bself->_net.send_message(ControlMessageType::VOICE_STATE_UPDATE, payload, 2);
    };

    _lobbyModel.on_select_capture = [bself](int index) {
        bself->_audio.set_capture_device(index);
        bself->_lobbyModel.selected_capture = index;
        bself->_lobbyModel.dirty("selected_capture");
    };

    _lobbyModel.on_select_playback = [bself](int index) {
        bself->_audio.set_playback_device(index);
        bself->_lobbyModel.selected_playback = index;
        bself->_lobbyModel.dirty("selected_playback");
    };

    _lobbyModel.on_denoise_changed = [bself](bool enabled) {
        bself->_audio.set_denoise_enabled(enabled);
    };

    _lobbyModel.on_normalize_changed = [bself](bool enabled) {
        bself->_audio.set_normalize_enabled(enabled);
    };

    _lobbyModel.on_normalize_target_changed = [bself](float target) {
        bself->_audio.set_normalize_target(target);
    };

    _lobbyModel.on_aec_changed = [bself](bool enabled) {
        bself->_audio.set_aec_enabled(enabled);
    };

    _lobbyModel.on_vad_changed = [bself](bool enabled) {
        bself->_audio.set_vad_enabled(enabled);
    };

    _lobbyModel.on_vad_threshold_changed = [bself](float threshold) {
        bself->_audio.set_vad_threshold(threshold);
    };

    _lobbyModel.on_toggle_ptt  = [bself]() {
        bool enabled = !bself->_lobbyModel.ptt_enabled;
        bself->_lobbyModel.ptt_enabled = enabled;
        bself->_lobbyModel.dirty("ptt_enabled");
        // PTT key binding is handled at the NSEvent level (keyCode match)
    };

    _lobbyModel.on_ptt_bind        = [bself]() { /* TODO: key binding on macOS */ };
    _lobbyModel.on_ptt_delay_changed = [](float) {};
    _lobbyModel.on_mute_bind       = [bself]() { /* TODO: global hotkey */ };
    _lobbyModel.on_deafen_bind     = [bself]() { /* TODO: global hotkey */ };

    _lobbyModel.on_toggle_share = [bself]() {
        if (bself->_sharing) {
            [bself stopScreenShare];
        } else {
            [bself showSharePicker];
        }
    };

    _lobbyModel.on_cancel_share = [bself]() {
        bself->_lobbyModel.show_share_picker = false;
        bself->_lobbyModel.dirty("show_share_picker");
    };

    _lobbyModel.on_start_native_share = [bself]() {
        [bself startNativeShare];
    };

    _lobbyModel.on_watch_sharer = [bself](int sharer_id) {
        [bself watchSharer:static_cast<UserId>(sharer_id)];
    };

    _lobbyModel.on_stop_watching = [bself]() {
        [bself stopWatching];
    };

    _lobbyModel.on_stream_volume_changed = [bself](float vol) {
        bself->_lobbyModel.stream_volume = vol;
    };

    // Identity
    _lobbyModel.on_show_seed_phrase = [bself]() {
        bself->_lobbyModel.identity_seed_phrase = Rml::String(bself->_seedPhrase);
        bself->_lobbyModel.show_seed_phrase = true;
        bself->_lobbyModel.dirty("identity_seed_phrase");
        bself->_lobbyModel.dirty("show_seed_phrase");
    };

    _lobbyModel.on_copy_seed_phrase = [bself]() {
        NSString* sp = [NSString stringWithUTF8String:bself->_seedPhrase.c_str()];
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:sp forType:NSPasteboardTypeString];
    };

    _lobbyModel.on_show_private_key = [bself]() {
        // Hex-encode the 32-byte secret key seed
        std::string hex;
        hex.reserve(64);
        static const char digits[] = "0123456789abcdef";
        for (uint8_t b : bself->_secretKey) {
            hex += digits[(b >> 4) & 0xF];
            hex += digits[b & 0xF];
        }
        bself->_lobbyModel.identity_private_key = Rml::String(hex);
        bself->_lobbyModel.show_private_key = true;
        bself->_lobbyModel.dirty("identity_private_key");
        bself->_lobbyModel.dirty("show_private_key");
    };

    _lobbyModel.on_copy_private_key = [bself]() {
        std::string hex;
        hex.reserve(64);
        static const char digits[] = "0123456789abcdef";
        for (uint8_t b : bself->_secretKey) {
            hex += digits[(b >> 4) & 0xF];
            hex += digits[b & 0xF];
        }
        NSString* s = [NSString stringWithUTF8String:hex.c_str()];
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:s forType:NSPasteboardTypeString];
    };

    _lobbyModel.on_show_import     = [bself]() {
        bself->_lobbyModel.show_import_identity = true;
        bself->_lobbyModel.import_phrase = "";
        bself->_lobbyModel.import_error  = "";
        bself->_lobbyModel.dirty("show_import_identity");
        bself->_lobbyModel.dirty("import_phrase");
        bself->_lobbyModel.dirty("import_error");
    };

    _lobbyModel.on_do_import = [bself]() {
        // TODO: parse 12/24 word BIP39 seed phrase and derive key pair
        bself->_lobbyModel.import_error = "Import not yet implemented";
        bself->_lobbyModel.dirty("import_error");
    };

    _lobbyModel.on_cancel_import = [bself]() {
        bself->_lobbyModel.show_import_identity = false;
        bself->_lobbyModel.dirty("show_import_identity");
    };

    // Admin
    _lobbyModel.on_create_channel = [bself]() {
        std::string name = bself->_lobbyModel.new_channel_name.c_str();
        if (name.empty()) return;
        uint8_t len8 = static_cast<uint8_t>(std::min(name.size(), (size_t)255));
        std::vector<uint8_t> payload(1 + len8);
        payload[0] = len8;
        std::memcpy(payload.data() + 1, name.data(), len8);
        bself->_net.send_message(ControlMessageType::ADMIN_CREATE_CHANNEL,
                                  payload.data(), payload.size());
        bself->_lobbyModel.show_create_channel = false;
        bself->_lobbyModel.new_channel_name    = "";
        bself->_lobbyModel.dirty("show_create_channel");
        bself->_lobbyModel.dirty("new_channel_name");
    };

    _lobbyModel.on_delete_channel = [bself](int channel_id) {
        uint32_t id32 = static_cast<uint32_t>(channel_id);
        bself->_net.send_message(ControlMessageType::ADMIN_DELETE_CHANNEL,
                                  (const uint8_t*)&id32, sizeof(id32));
    };

    _lobbyModel.on_show_user_menu = [bself](int user_id, std::string name, int role) {
        bself->_lobbyModel.show_user_menu   = true;
        bself->_lobbyModel.menu_user_id     = user_id;
        bself->_lobbyModel.menu_user_name   = Rml::String(name);
        bself->_lobbyModel.menu_user_role   = role;
        bself->_lobbyModel.menu_can_roles   = (bself->_myRole <= static_cast<int>(parties::Role::Admin));
        bself->_lobbyModel.menu_can_kick    = (bself->_myRole <= static_cast<int>(parties::Role::Moderator));
        float vol = bself->_mixer.get_user_volume(static_cast<UserId>(user_id));
        bself->_lobbyModel.menu_user_volume = vol;
        bself->_lobbyModel.dirty("show_user_menu");
        bself->_lobbyModel.dirty("menu_user_id");
        bself->_lobbyModel.dirty("menu_user_name");
        bself->_lobbyModel.dirty("menu_user_role");
        bself->_lobbyModel.dirty("menu_user_volume");
        bself->_lobbyModel.dirty("menu_can_roles");
        bself->_lobbyModel.dirty("menu_can_kick");
    };

    _lobbyModel.on_set_user_role = [bself](int user_id, int new_role) {
        uint8_t payload[5];
        uint32_t id32 = static_cast<uint32_t>(user_id);
        std::memcpy(payload, &id32, 4);
        payload[4] = static_cast<uint8_t>(new_role);
        bself->_net.send_message(ControlMessageType::ADMIN_SET_ROLE, payload, 5);
    };

    _lobbyModel.on_kick_user = [bself](int user_id) {
        uint32_t id32 = static_cast<uint32_t>(user_id);
        bself->_net.send_message(ControlMessageType::ADMIN_KICK_USER,
                                  (const uint8_t*)&id32, sizeof(id32));
    };

    _lobbyModel.on_user_volume_changed = [bself](int user_id, float volume) {
        bself->_mixer.set_user_volume(static_cast<UserId>(user_id), volume);
    };

    _lobbyModel.on_user_compress_changed = [bself](int user_id, bool enabled, float target) {
        bself->_mixer.set_user_compression(static_cast<UserId>(user_id), enabled, target);
    };

    _lobbyModel.on_select_sharer = [bself](int sharer_id) {
        [bself watchSharer:static_cast<UserId>(sharer_id)];
    };
}

- (void)shutdown
{
    // Disconnect the network client so that all MsQuic handles
    // (connection, configuration, registration) are closed and fully
    // drained before the caller invokes quic_cleanup() / MsQuicClose.
    // RegistrationClose is synchronous — it blocks until all connections
    // under it have shut down, satisfying MsQuicLibraryUninitialize's
    // assertion that no registrations remain at close time.
    _net.disconnect();
    _audio.stop();
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
    _window.title                 = @"Parties";
    _window.contentViewController = _viewController;
    _window.minSize               = NSMakeSize(800, 500);

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
    // Shut down the net client first: closes the MsQuic registration and
    // blocks until all connections drain. quic_cleanup() / MsQuicClose must
    // be called only after all MsQuic handles are released, otherwise
    // MsQuicLibraryUninitialize fires quic_bugcheck (SIGABRT).
    [_viewController shutdown];
    Rml::Shutdown();
    Backend::Shutdown();
    parties::quic_cleanup();
}

@end
