#import "PartiesViewController.h"
#import <QuartzCore/CABase.h>   // CACurrentMediaTime()
#import <AVFoundation/AVFoundation.h>

// RmlUi Metal backend
#import "../Backends/RmlUi_Backend_iOS_Metal.h"

// RmlUi core
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

// Parties protocol
#include <parties/protocol.h>
#include <parties/types.h>
#include <parties/serialization.h>
#include <parties/audio_common.h>

// Shared client code
#include <client/net_client.h>
#include <client/audio_engine.h>
#include <client/voice_mixer.h>

// iOS UI data model
#include "app_model.h"

using namespace parties;
using namespace parties::client;
using namespace parties::protocol;

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

    // App state
    UserId                  _my_user_id;
    ChannelId               _current_channel;
    ChannelKey              _current_channel_key;

    // UI data model
    LobbyModel*             _model;

    // Keepalive timer (30s interval)
    NSTimer*                _keepalive_timer;
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

    // Font.
    NSString* font_path = [[NSBundle mainBundle] pathForResource:@"LatoLatin-Regular"
                                                           ofType:@"ttf"];
    if (font_path)
        Rml::LoadFontFace(font_path.UTF8String);
    else
        NSLog(@"[Parties] LatoLatin-Regular.ttf not found in bundle.");

    // Parties networking + audio.
    _net_client  = new NetClient();
    _voice_mixer = new VoiceMixer();
    _audio_engine = new AudioEngine();
    _audio_engine->set_mixer(_voice_mixer);
    _audio_engine->on_encoded_frame = [self](const uint8_t* data, size_t len) {
        [self sendVoiceFrame:data length:len];
    };
    _audio_engine->init();

    // Data model + document.
    _model = new LobbyModel();
    [self loadDocument];

    _model->setup(_context);
    [self bindModelEvents];

    _channels_el = _document ? _document->GetElementById("channels") : nullptr;
}

- (void)loadDocument
{
    NSString* path = [[NSBundle mainBundle] pathForResource:@"lobby" ofType:@"rml"];
    if (path) {
        _document = _context->LoadDocument(path.UTF8String);
        if (_document) _document->Show();
    } else {
        NSLog(@"[Parties] lobby.rml not found in bundle.");
    }
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
    };

    _model->on_leave_channel = [self]() {
        if (!_net_client->is_connected()) return;
        _net_client->send_message(ControlMessageType::CHANNEL_LEAVE, nullptr, 0);
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
        _model->mark_dirty();
    };

    _model->on_toggle_deafen = [self]() {
        bool deafened = !_audio_engine->is_deafened();
        _audio_engine->set_deafened(deafened);
        _model->is_deafened = deafened;
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

- (void)onDisconnected
{
    [_keepalive_timer invalidate];
    _keepalive_timer = nil;
    _audio_engine->stop();
    _voice_mixer->clear();
    _current_channel = 0;
    _model->is_connected = false;
    _model->show_login   = true;
    _model->login_status = "Disconnected.";
    _model->channels.clear();
    _model->mark_dirty();
}

// ── Server message dispatch ────────────────────────────────────────────────────

- (void)processServerMessages
{
    ServerMessage msg;
    while (_net_client->incoming().try_pop(msg)) {
        BinaryReader r(msg.payload.data(), msg.payload.size());
        [self handleMessage:msg.type reader:r];
    }
}

- (void)handleMessage:(ControlMessageType)type reader:(BinaryReader&)r
{
    switch (type) {

    case ControlMessageType::AUTH_RESPONSE: {
        _my_user_id = r.read_u32();
        EnetToken enet_token;
        r.read_bytes(enet_token.data(), 32);
        SessionToken session_token;
        r.read_bytes(session_token.data(), 32);
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

        std::vector<uint8_t> plaintext(ct_len);
        bool ok = voice_decrypt(_current_channel_key.data(), nonce,
                                nullptr, 0,
                                ciphertext, ct_len, tag,
                                plaintext.data());
        if (ok)
            _voice_mixer->push_packet(sender_id, plaintext.data(), ct_len);
    }
}

- (void)sendVoiceFrame:(const uint8_t*)opus_data length:(size_t)len
{
    if (!_net_client->is_connected() || _current_channel == 0 || _audio_engine->is_muted())
        return;

    // Build: [0x01][my_user_id(4)][nonce(12)][tag(16)][ciphertext]
    static uint64_t nonce_counter = 0;
    uint8_t nonce[12] = {};
    memcpy(nonce, &nonce_counter, sizeof(nonce_counter));
    nonce_counter++;

    std::vector<uint8_t> ciphertext(len);
    uint8_t tag[16];
    if (!voice_encrypt(_current_channel_key.data(), nonce, nullptr, 0,
                       opus_data, len, ciphertext.data(), tag))
        return;

    std::vector<uint8_t> packet(1 + 4 + 12 + 16 + len);
    packet[0] = VOICE_PACKET_TYPE;
    memcpy(packet.data() + 1,  &_my_user_id, 4);
    memcpy(packet.data() + 5,  nonce, 12);
    memcpy(packet.data() + 17, tag, 16);
    memcpy(packet.data() + 33, ciphertext.data(), len);
    _net_client->send_data(packet.data(), packet.size(), /*reliable=*/false);
}

- (void)sendKeepalive
{
    if (_net_client->is_connected())
        _net_client->send_message(ControlMessageType::KEEPALIVE_PING, nullptr, 0);
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

// ── Teardown ──────────────────────────────────────────────────────────────────

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_keepalive_timer invalidate];

    if (_audio_engine) { _audio_engine->stop(); _audio_engine->shutdown(); }
    if (_net_client)   { _net_client->disconnect(); }

    delete _model;        _model        = nullptr;
    delete _audio_engine; _audio_engine = nullptr;
    delete _voice_mixer;  _voice_mixer  = nullptr;
    delete _net_client;   _net_client   = nullptr;

    if (_context) {
        Rml::RemoveContext(_context->GetName());
        _context = nullptr;
    }
    Rml::Shutdown();
    Backend::Shutdown();
}

// ── MTKViewDelegate ───────────────────────────────────────────────────────────

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    Backend::SetViewport((int)size.width, (int)size.height);
    if (_context)
        _context->SetDimensions(Rml::Vector2i((int)size.width, (int)size.height));
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
            Backend::SetViewport((int)colorTex.width, (int)colorTex.height);
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
    return { (float)(p.x * _dp_ratio), (float)(p.y * _dp_ratio) };
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
