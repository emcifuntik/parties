#include <client/app_core.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/log.h>

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace parties::client {

AppCore::AppCore() = default;
AppCore::~AppCore() = default;

// ─────────────────────────────────────────────────────────────────────────────
// init / shutdown
// ─────────────────────────────────────────────────────────────────────────────

bool AppCore::init(const std::string& settings_path, PlatformBridge bridge, Rml::Context* rml_context)
{
    bridge_ = std::move(bridge);

    if (!settings_.open(settings_path)) {
        LOG_ERROR("Failed to open settings: {}", settings_path);
    }

    // Wire audio subsystems
    audio_.set_mixer(&mixer_);
    audio_.set_stream_player(&stream_audio_player_);

    audio_.on_encoded_frame = [this](const uint8_t* data, size_t len) {
        if (!authenticated_ || current_channel_ == 0 || audio_.is_muted()) return;
        uint16_t seq = voice_seq_++;
        std::vector<uint8_t> pkt(1 + 2 + len);
        pkt[0] = protocol::VOICE_PACKET_TYPE;
        std::memcpy(pkt.data() + 1, &seq, 2);
        std::memcpy(pkt.data() + 3, data, len);
        net_.send_data(pkt.data(), pkt.size());
    };

    // Wire net callbacks
    net_.on_disconnected = [this]() { on_disconnect_cleanup(); };

    net_.on_data_received = [this](const uint8_t* data, size_t len) {
        if (len < 1) return;
        uint8_t type = data[0];

        if (type == protocol::VOICE_PACKET_TYPE) {
            // [type(1)][sender_id(4)][seq(2)][opus(N)]
            if (len < 8) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            uint16_t seq;
            std::memcpy(&seq, data + 5, 2);
            mixer_.push_packet(sender_id, seq, data + 7, len - 7);
        } else if (type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            // [type(1)][sender_id(4)][fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][data(N)]
            if (len < 19) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            if (on_video_frame_received)
                on_video_frame_received(sender_id, data + 5, len - 5);
        } else if (type == protocol::STREAM_AUDIO_PACKET_TYPE) {
            // [type(1)][sender_id(4)][opus(N)]
            if (len < 6) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            if (sender_id == viewing_sharer_)
                stream_audio_player_.push_packet(data + 5, len - 5);
        } else if (type == protocol::VIDEO_CONTROL_TYPE) {
            if (len >= 2 && data[1] == protocol::VIDEO_CTL_PLI && bridge_.request_keyframe)
                bridge_.request_keyframe();
        }
    };

    net_.on_resumption_ticket = [this](const uint8_t* ticket, size_t len) {
        settings_.save_resumption_ticket(server_host_, server_port_, ticket, len);
    };

    // Apply saved per-user prefs whenever the mixer creates a new stream
    mixer_.on_stream_created = [this](UserId uid) { apply_user_audio_prefs(uid); };

    // Setup model callbacks
    setup_model_callbacks();
    setup_server_model_callbacks();

    // Initialize models with RmlUi context
    if (!server_model_.init(rml_context)) {
        LOG_ERROR("server_model init failed");
        return false;
    }
    if (!model_.init(rml_context)) {
        LOG_ERROR("model init failed");
        return false;
    }

    // Initialize audio
    audio_.init();
    stream_audio_player_.init();

    return true;
}

void AppCore::shutdown()
{
    net_.disconnect();
    audio_.stop();
    stream_audio_player_.shutdown();
    flush_pending_prefs(true);
    settings_.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-frame tick
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::tick()
{
    if (awaiting_connection_) poll_connecting();
    process_server_messages();
    update_speaking_state();
    flush_pending_prefs();

    // Stream FPS counter (updated once per second)
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - stream_fps_last_update_).count();
    if (elapsed >= 1.0f) {
        uint32_t sc = stream_frame_count_.exchange(0, std::memory_order_relaxed);
        int sfps = static_cast<int>(sc / elapsed);
        if (sfps != model_.stream_fps) {
            model_.stream_fps = sfps;
            model_.dirty("stream_fps");
        }
        stream_fps_last_update_ = now;
    }

    // Update local mic level indicator
    if (authenticated_ && current_channel_ != 0) {
        float lvl = audio_.voice_level();
        if (std::abs(lvl - model_.voice_level) > 0.01f) {
            model_.voice_level = lvl;
            model_.dirty("voice_level");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// load_saved_prefs — call from platform init after AppCore::init()
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::load_saved_prefs()
{
    auto pref = [&](const char* k) -> std::string {
        auto v = settings_.get_pref(k);
        return v.value_or("");
    };

    auto capture_devs  = audio_.get_capture_devices();
    auto playback_devs = audio_.get_playback_devices();

    for (auto& d : capture_devs)
        model_.capture_devices.push_back({Rml::String(d.name), d.index});
    for (auto& d : playback_devs)
        model_.playback_devices.push_back({Rml::String(d.name), d.index});

    model_.selected_capture  = audio_.default_capture_index();
    model_.selected_playback = audio_.default_playback_index();

    std::string v;

    v = pref("audio.denoise");
    if (!v.empty()) { bool e = (v != "0"); audio_.set_denoise_enabled(e); model_.denoise_enabled = e; }

    v = pref("audio.normalize");
    if (!v.empty()) { bool e = (v != "0"); audio_.set_normalize_enabled(e); model_.normalize_enabled = e; }

    v = pref("audio.normalize_target");
    if (!v.empty()) { float t = std::strtof(v.c_str(), nullptr); audio_.set_normalize_target(t); model_.normalize_target = t; }

    v = pref("audio.aec");
    if (!v.empty()) { bool e = (v != "0"); audio_.set_aec_enabled(e); model_.aec_enabled = e; }

    v = pref("audio.vad");
    if (!v.empty()) { bool e = (v != "0"); audio_.set_vad_enabled(e); model_.vad_enabled = e; }

    v = pref("audio.vad_threshold");
    if (!v.empty()) { float t = std::strtof(v.c_str(), nullptr); audio_.set_vad_threshold(t); model_.vad_threshold = t; }

    v = pref("audio.ptt");
    if (!v.empty()) model_.ptt_enabled = (v != "0");

    v = pref("audio.ptt_delay");
    if (!v.empty()) model_.ptt_delay = static_cast<float>(std::stoi(v));

    v = pref("audio.stream_volume");
    if (!v.empty()) { float vol = std::strtof(v.c_str(), nullptr); stream_audio_player_.set_volume(vol); model_.stream_volume = vol; }

    v = pref("video.share_bitrate");
    if (!v.empty()) model_.share_bitrate = std::strtof(v.c_str(), nullptr);

    v = pref("video.share_fps");
    if (!v.empty()) model_.share_fps = std::atoi(v.c_str());

    // Restore saved device by name
    v = pref("audio.capture_device");
    if (!v.empty()) {
        for (size_t i = 0; i < capture_devs.size(); i++) {
            if (capture_devs[i].name == v) {
                audio_.set_capture_device(static_cast<int>(i));
                model_.selected_capture = static_cast<int>(i);
                break;
            }
        }
    }

    v = pref("audio.playback_device");
    if (!v.empty()) {
        for (size_t i = 0; i < playback_devs.size(); i++) {
            if (playback_devs[i].name == v) {
                audio_.set_playback_device(static_cast<int>(i));
                model_.selected_playback = static_cast<int>(i);
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Identity
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::load_or_generate_identity(const std::string& username_hint)
{
    auto identity = settings_.load_identity();
    if (identity) {
        seed_phrase_ = identity->seed_phrase;
        secret_key_  = identity->secret_key;
        public_key_  = identity->public_key;
        has_identity_ = true;
    } else {
        generate_identity();
    }

    if (!username_hint.empty())
        username_ = username_hint;

    server_model_.has_identity = has_identity_;
    server_model_.fingerprint  = Rml::String(settings_.get_fingerprint());
    server_model_.dirty("has_identity");
    server_model_.dirty("fingerprint");
}

void AppCore::generate_identity()
{
    seed_phrase_ = parties::generate_seed_phrase();
    parties::derive_keypair(seed_phrase_, secret_key_, public_key_);
    has_identity_ = true;
    settings_.save_identity(seed_phrase_, secret_key_, public_key_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server list
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::refresh_server_list()
{
    auto saved = settings_.get_saved_servers();
    server_model_.servers.clear();
    for (auto& s : saved) {
        ServerEntry entry;
        entry.id            = s.id;
        entry.name          = Rml::String(s.name);
        entry.host          = Rml::String(s.host);
        entry.port          = s.port;
        entry.last_username = Rml::String(s.last_username);
        if (s.name.size() >= 2)
            entry.initials = Rml::String(s.name.substr(0, 2));
        else if (!s.name.empty())
            entry.initials = Rml::String(s.name.substr(0, 1));
        else
            entry.initials = "?";
        // Color index from simple name hash (0-4)
        uint32_t hash = 0;
        for (char c : s.name) hash = hash * 31 + static_cast<uint8_t>(c);
        entry.color_index = static_cast<int>(hash % 5);
        server_model_.servers.push_back(std::move(entry));
    }
    size_t n = server_model_.servers.size();
    server_model_.party_count_text = Rml::String(
        std::to_string(n) + (n == 1 ? " party" : " parties"));
    server_model_.dirty("servers");
    server_model_.dirty("party_count_text");
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection flow
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::do_connect()
{
    if (!has_identity_) {
        server_model_.login_error = "No identity — generate seed phrase first";
        server_model_.dirty("login_error");
        return;
    }

    username_ = server_model_.login_username;
    server_model_.login_error = "";
    server_model_.dirty("login_error");

    if (net_.is_connected()) { finish_connect(); return; }
    if (net_.is_connecting()) return;

    server_model_.login_status = "Connecting...";
    server_model_.dirty("login_status");

    auto ticket = settings_.load_resumption_ticket(server_host_, server_port_);
    if (!net_.connect(server_host_, server_port_,
                      ticket.empty() ? nullptr : ticket.data(), ticket.size())) {
        server_model_.login_error = "Failed to connect to server";
        server_model_.dirty("login_error");
        return;
    }
    awaiting_connection_ = true;
}

void AppCore::poll_connecting()
{
    if (net_.connect_failed()) {
        awaiting_connection_ = false;
        server_model_.login_error  = "Failed to connect to server";
        server_model_.login_status = "";
        server_model_.dirty("login_error");
        server_model_.dirty("login_status");
        net_.disconnect();
        return;
    }
    if (net_.is_connected()) {
        awaiting_connection_ = false;
        finish_connect();
    }
}

void AppCore::finish_connect()
{
    std::string fp = net_.get_server_fingerprint();

    // On TLS 1.3 session resumption (0-RTT), no certificate is exchanged
    // (PSK authentication), so the fingerprint is empty. The server accepted
    // our session ticket, confirming it's the same server — skip TOFU check.
    if (!fp.empty()) {
        auto result = settings_.check_fingerprint(server_host_, server_port_, fp);

        if (result == Settings::TofuResult::Mismatch) {
            tofu_pending_fingerprint_ = fp;
            tofu_pending_ = true;
            server_model_.tofu_fingerprint  = Rml::String(fp);
            server_model_.show_tofu_warning = true;
            server_model_.show_login        = false;
            server_model_.dirty("tofu_fingerprint");
            server_model_.dirty("show_tofu_warning");
            server_model_.dirty("show_login");
            return;
        }
        if (result == Settings::TofuResult::Unknown)
            settings_.trust_fingerprint(server_host_, server_port_, fp);
    }

    server_model_.login_status = "Authenticating...";
    server_model_.dirty("login_status");
    send_auth_identity();
}

void AppCore::send_auth_identity()
{
    if (!has_identity_) return;

    auto now = static_cast<uint64_t>(std::time(nullptr));

    BinaryWriter sig_msg;
    sig_msg.write_bytes(public_key_.data(), public_key_.size());
    sig_msg.write_string(username_);
    sig_msg.write_u64(now);

    parties::Signature sig{};
    if (!parties::ed25519_sign(sig_msg.data().data(), sig_msg.data().size(),
                                secret_key_, public_key_, sig)) {
        server_model_.login_error = "Failed to sign auth message";
        server_model_.dirty("login_error");
        return;
    }

    BinaryWriter writer;
    writer.write_u16(protocol::PROTOCOL_VERSION);
    writer.write_bytes(public_key_.data(), public_key_.size());
    writer.write_string(username_);
    writer.write_u64(now);
    writer.write_bytes(sig.data(), sig.size());

    net_.send_message(protocol::ControlMessageType::AUTH_IDENTITY,
                      writer.data().data(), writer.data().size());
}

void AppCore::on_disconnect_cleanup()
{
    authenticated_ = false;
    current_channel_ = 0;
    channel_key_ = {};
    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    video_frame_number_ = 0;
    awaiting_connection_ = false;
    awaiting_channel_join_ = false;

    audio_.stop();
    mixer_.clear();
    clear_all_sharers();
    voice_last_active_.clear();

    model_.is_connected = false;
    model_.current_channel = 0;
    model_.current_channel_name.clear();
    model_.channels.clear();
    model_.is_muted = false;
    model_.is_deafened = false;
    model_.is_sharing = false;
    model_.show_settings = false;
    model_.show_share_picker = false;
    model_.show_create_channel = false;
    model_.my_role = 3;
    model_.can_manage_channels = false;
    model_.can_kick = false;
    model_.can_manage_roles = false;
    model_.admin_message.clear();
    model_.dirty_all();

    server_model_.connected_server_id = 0;
    server_model_.show_login          = false;
    server_model_.show_add_form       = false;
    server_model_.login_error         = "";
    server_model_.login_status        = "";
    server_model_.dirty("connected_server_id");
    server_model_.dirty("show_login");
    server_model_.dirty("show_add_form");
    server_model_.dirty("login_error");
    server_model_.dirty("login_status");
}

// ─────────────────────────────────────────────────────────────────────────────
// Channel operations
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::join_channel(ChannelId id)
{
    if (!authenticated_ || id == current_channel_) return;
    awaiting_channel_join_ = true;
    pending_channel_id_ = id;
    BinaryWriter w;
    w.write_u32(id);
    net_.send_message(protocol::ControlMessageType::CHANNEL_JOIN,
                      w.data().data(), w.data().size());
}

void AppCore::leave_channel()
{
    if (!authenticated_ || current_channel_ == 0) return;

    if (model_.show_share_picker) {
        model_.show_share_picker = false;
        model_.dirty("show_share_picker");
    }
    if (model_.is_sharing && bridge_.stop_screen_share)
        bridge_.stop_screen_share();

    clear_all_sharers();
    model_.is_sharing = false;
    model_.dirty("is_sharing");

    net_.send_message(protocol::ControlMessageType::CHANNEL_LEAVE, nullptr, 0);

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(current_channel_)) {
            auto& u = ch.users;
            u.erase(std::remove_if(u.begin(), u.end(),
                [this](const ChannelUser& cu) { return cu.id == static_cast<int>(user_id_); }),
                u.end());
            ch.user_count = static_cast<int>(u.size());
            break;
        }
    }

    current_channel_ = 0;
    channel_key_ = {};

    if (bridge_.play_sound)
        bridge_.play_sound(SoundPlayer::Effect::LeaveChannel);
    audio_.stop();
    mixer_.clear();

    model_.current_channel = 0;
    model_.current_channel_name.clear();
    model_.dirty("current_channel");
    model_.dirty("current_channel_name");
    model_.dirty("channels");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen share helpers
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::watch_sharer(UserId id)
{
    viewing_sharer_ = id;
    awaiting_keyframe_ = true;
    send_pli(id);
    if (bridge_.start_decode_thread)
        bridge_.start_decode_thread();
    uint32_t id32 = id;
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      reinterpret_cast<const uint8_t*>(&id32), 4);
    model_.viewing_sharer_id = static_cast<int>(id);
    // Don't dirty yet — platform dirties viewing_sharer_id when the first
    // decoded frame is displayed, so the video area appears with content
    // instead of flashing a black frame.
}

void AppCore::stop_watching()
{
    if (bridge_.stop_decode_thread)
        bridge_.stop_decode_thread();
    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    uint32_t zero = 0;
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      reinterpret_cast<const uint8_t*>(&zero), 4);
    model_.viewing_sharer_id = 0;
    model_.dirty("viewing_sharer_id");
    if (bridge_.clear_video_element)
        bridge_.clear_video_element();
}

void AppCore::send_pli(UserId target)
{
    std::vector<uint8_t> pkt(6);
    pkt[0] = protocol::VIDEO_CONTROL_TYPE;
    pkt[1] = protocol::VIDEO_CTL_PLI;
    std::memcpy(pkt.data() + 2, &target, 4);
    net_.send_video(pkt.data(), pkt.size(), true);
}

void AppCore::clear_all_sharers()
{
    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    active_sharers_.clear();
    model_.sharers.clear();
    model_.someone_sharing = false;
    model_.viewing_sharer_id = 0;
    model_.dirty("sharers");
    model_.dirty("someone_sharing");
    model_.dirty("viewing_sharer_id");
}

// ─────────────────────────────────────────────────────────────────────────────
// Voice state
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::send_voice_state()
{
    if (!authenticated_ || current_channel_ == 0) return;
    uint8_t payload[2] = {
        static_cast<uint8_t>(model_.is_muted ? 1 : 0),
        static_cast<uint8_t>(model_.is_deafened ? 1 : 0)
    };
    net_.send_message(protocol::ControlMessageType::VOICE_STATE_UPDATE, payload, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Network message dispatch
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::process_server_messages()
{
    auto messages = net_.incoming().drain();
    for (auto& msg : messages)
        handle_server_message(msg.type, msg.payload.data(), msg.payload.size());
}

void AppCore::handle_server_message(protocol::ControlMessageType type,
                                     const uint8_t* data, size_t len)
{
    switch (type) {
    case protocol::ControlMessageType::AUTH_RESPONSE:
        on_auth_response(data, len); break;
    case protocol::ControlMessageType::CHANNEL_LIST:
        on_channel_list(data, len); break;
    case protocol::ControlMessageType::CHANNEL_USER_LIST:
        on_channel_user_list(data, len); break;
    case protocol::ControlMessageType::USER_JOINED_CHANNEL:
        on_user_joined(data, len); break;
    case protocol::ControlMessageType::USER_LEFT_CHANNEL:
        on_user_left(data, len); break;
    case protocol::ControlMessageType::USER_VOICE_STATE:
        on_user_voice_state(data, len); break;
    case protocol::ControlMessageType::USER_ROLE_CHANGED:
        on_user_role_changed(data, len); break;
    case protocol::ControlMessageType::CHANNEL_KEY:
        on_channel_key(data, len); break;
    case protocol::ControlMessageType::SCREEN_SHARE_STARTED:
        on_screen_share_started(data, len); break;
    case protocol::ControlMessageType::SCREEN_SHARE_STOPPED:
        on_screen_share_stopped(data, len); break;
    case protocol::ControlMessageType::SCREEN_SHARE_DENIED:
        on_screen_share_denied(data, len); break;
    case protocol::ControlMessageType::ADMIN_RESULT:
        on_admin_result(data, len); break;
    case protocol::ControlMessageType::SERVER_ERROR:
        on_server_error(data, len); break;
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Protocol handlers
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::on_auth_response(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    user_id_ = reader.read_u32();
    uint8_t session_token[32];
    reader.read_bytes(session_token, 32);
    role_ = reader.read_u8();
    std::string server_name = reader.read_string();
    if (reader.error()) return;

    authenticated_ = true;

    settings_.save_server(server_name, server_host_, server_port_,
                          net_.get_server_fingerprint(), username_);
    refresh_server_list();

    if (bridge_.on_authenticated) bridge_.on_authenticated();

    model_.server_name          = Rml::String(server_name);
    // Compute server initials and color
    if (server_name.size() >= 2)
        model_.server_initials = Rml::String(server_name.substr(0, 2));
    else if (!server_name.empty())
        model_.server_initials = Rml::String(server_name.substr(0, 1));
    else
        model_.server_initials = "?";
    {
        uint32_t h = 0;
        for (char c : server_name) h = h * 31 + static_cast<uint8_t>(c);
        model_.server_color_index = static_cast<int>(h % 5);
    }
    model_.username             = Rml::String(username_);
    { uint32_t h = 0; for (char c : username_) h = h * 31 + static_cast<uint8_t>(c); model_.my_color_index = static_cast<int>(h % 12); }
    model_.is_connected         = true;
    model_.my_role              = role_;
    model_.can_manage_channels  = (role_ <= static_cast<int>(parties::Role::Moderator));
    model_.can_kick             = (role_ <= static_cast<int>(parties::Role::Moderator));
    model_.can_manage_roles     = (role_ <= static_cast<int>(parties::Role::Admin));
    model_.dirty("server_name");
    model_.dirty("server_initials");
    model_.dirty("server_color_index");
    model_.dirty("username");
    model_.dirty("my_color_index");
    model_.dirty("is_connected");
    model_.dirty("my_role");
    model_.dirty("can_manage_channels");
    model_.dirty("can_kick");
    model_.dirty("can_manage_roles");

    server_model_.connected_server_id = connecting_server_id_;
    server_model_.show_login          = false;
    server_model_.login_error         = "";
    server_model_.login_status        = "";
    server_model_.dirty("connected_server_id");
    server_model_.dirty("show_login");
    server_model_.dirty("login_error");
    server_model_.dirty("login_status");

    // Populate audio device lists
    auto caps = audio_.get_capture_devices();
    model_.capture_devices.clear();
    for (auto& d : caps)
        model_.capture_devices.push_back({Rml::String(d.name), d.index});
    auto plays = audio_.get_playback_devices();
    model_.playback_devices.clear();
    for (auto& d : plays)
        model_.playback_devices.push_back({Rml::String(d.name), d.index});
    model_.dirty("capture_devices");
    model_.dirty("playback_devices");
}

void AppCore::on_channel_list(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    std::unordered_map<int, Rml::Vector<ChannelUser>> old_users;
    for (auto& ch : model_.channels)
        old_users[ch.id] = std::move(ch.users);

    model_.channels.clear();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ch_id    = reader.read_u32();
        std::string name  = reader.read_string();
        uint32_t max_u    = reader.read_u32();
        uint32_t sort_ord = reader.read_u32();
        uint32_t user_cnt = reader.read_u32();
        if (reader.error()) break;
        (void)sort_ord;

        ChannelInfo ch;
        ch.id         = static_cast<int>(ch_id);
        ch.name       = Rml::String(name);
        ch.max_users  = static_cast<int>(max_u);
        ch.user_count = static_cast<int>(user_cnt);

        auto it = old_users.find(ch.id);
        if (it != old_users.end()) {
            ch.users = std::move(it->second);
            // For our current channel, user list is authoritative;
            // for other channels, trust the server's count
            if (static_cast<uint32_t>(ch.id) == current_channel_)
                ch.user_count = static_cast<int>(ch.users.size());
        }
        model_.channels.push_back(std::move(ch));
    }
    model_.dirty("channels");
}

void AppCore::on_channel_user_list(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    ChannelId channel_id = reader.read_u32();
    uint32_t count       = reader.read_u32();
    if (reader.error()) return;

    Rml::Vector<ChannelUser> users;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t uid      = reader.read_u32();
        std::string uname = reader.read_string();
        uint8_t urole     = reader.read_u8();
        uint8_t muted     = reader.read_u8();
        uint8_t deaf      = reader.read_u8();
        if (reader.error()) break;

        ChannelUser u;
        u.id       = static_cast<int>(uid);
        u.name     = Rml::String(uname);
        u.role     = urole;
        u.muted    = (muted != 0);
        u.deafened = (deaf  != 0);
        { uint32_t h = 0; for (char c : uname) h = h * 31 + static_cast<uint8_t>(c); u.color_index = static_cast<int>(h % 12); }
        users.push_back(u);
    }

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            ch.users      = users;
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }

    if (awaiting_channel_join_ && pending_channel_id_ == channel_id) {
        awaiting_channel_join_ = false;

        // Remove self from old channel
        if (current_channel_ != 0 && current_channel_ != channel_id) {
            for (auto& ch : model_.channels) {
                if (ch.id == static_cast<int>(current_channel_)) {
                    auto& u = ch.users;
                    u.erase(std::remove_if(u.begin(), u.end(),
                        [this](const ChannelUser& cu) { return cu.id == static_cast<int>(user_id_); }),
                        u.end());
                    ch.user_count = static_cast<int>(u.size());
                    break;
                }
            }
        }

        current_channel_ = channel_id;
        model_.current_channel = static_cast<int>(channel_id);

        for (auto& ch : model_.channels) {
            if (ch.id == static_cast<int>(channel_id)) {
                model_.current_channel_name = ch.name;
                for (auto& u : ch.users)
                    if (static_cast<uint32_t>(u.id) != user_id_)
                        apply_user_audio_prefs(static_cast<UserId>(u.id));
                break;
            }
        }
        model_.dirty("current_channel");
        model_.dirty("current_channel_name");
        audio_.start();
        if (bridge_.play_sound)
            bridge_.play_sound(SoundPlayer::Effect::JoinChannel);
    }
    model_.dirty("channels");
}

void AppCore::on_user_joined(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    std::string uname   = reader.read_string();
    uint32_t channel_id = reader.read_u32();
    uint8_t urole       = reader.has_remaining(1) ? reader.read_u8() : 3;
    if (reader.error()) return;

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            // Remove existing entry for this user (handles identity takeover / rejoin)
            auto& ul = ch.users;
            ul.erase(std::remove_if(ul.begin(), ul.end(),
                [uid](const ChannelUser& cu) { return cu.id == static_cast<int>(uid); }),
                ul.end());

            ChannelUser u;
            u.id   = static_cast<int>(uid);
            u.name = Rml::String(uname);
            u.role = urole;
            { uint32_t h = 0; for (char c : uname) h = h * 31 + static_cast<uint8_t>(c); u.color_index = static_cast<int>(h % 12); }
            ch.users.push_back(u);
            ch.user_count = static_cast<int>(ch.users.size());
            break;
        }
    }
    if (channel_id == current_channel_) {
        if (bridge_.play_sound)
            bridge_.play_sound(SoundPlayer::Effect::UserJoined);
        apply_user_audio_prefs(uid);
    }
    model_.dirty("channels");
}

void AppCore::on_user_left(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    mixer_.remove_user(uid);

    for (auto& ch : model_.channels) {
        if (ch.id == static_cast<int>(channel_id)) {
            auto& u = ch.users;
            u.erase(std::remove_if(u.begin(), u.end(),
                [uid](const ChannelUser& cu) { return cu.id == static_cast<int>(uid); }),
                u.end());
            ch.user_count = static_cast<int>(u.size());
            break;
        }
    }
    if (channel_id == current_channel_)
        if (bridge_.play_sound)
            bridge_.play_sound(SoundPlayer::Effect::UserLeft);
    model_.dirty("channels");
}

void AppCore::on_user_voice_state(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid   = reader.read_u32();
    uint8_t muted  = reader.read_u8();
    uint8_t deaf   = reader.read_u8();
    if (reader.error()) return;

    for (auto& ch : model_.channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(uid)) {
                u.muted    = (muted != 0);
                u.deafened = (deaf  != 0);
            }
    model_.dirty("channels");
}

void AppCore::on_user_role_changed(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid      = reader.read_u32();
    uint8_t new_role  = reader.read_u8();
    if (reader.error()) return;

    if (uid == user_id_) {
        role_                   = new_role;
        model_.my_role          = new_role;
        model_.can_manage_channels = (new_role <= static_cast<int>(parties::Role::Moderator));
        model_.can_kick            = (new_role <= static_cast<int>(parties::Role::Moderator));
        model_.can_manage_roles    = (new_role <= static_cast<int>(parties::Role::Admin));
        model_.dirty("my_role");
        model_.dirty("can_manage_channels");
        model_.dirty("can_kick");
        model_.dirty("can_manage_roles");
    }
    for (auto& ch : model_.channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(uid))
                u.role = new_role;
    model_.dirty("channels");
}

void AppCore::on_channel_key(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    ChannelId ch_id = reader.read_u32();
    (void)ch_id;
    if (reader.remaining() < 32 || reader.error()) return;
    reader.read_bytes(channel_key_.data(), 32);
}

void AppCore::on_screen_share_started(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t sharer_id = reader.read_u32();
    if (reader.error()) return;

    std::string sharer_name = "Unknown";
    for (auto& ch : model_.channels)
        if (ch.id == static_cast<int>(current_channel_))
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(sharer_id)) { sharer_name = u.name.c_str(); break; }

    ActiveSharer s;
    s.id   = static_cast<int>(sharer_id);
    s.name = Rml::String(sharer_name);

    auto it = std::remove_if(model_.sharers.begin(), model_.sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    model_.sharers.erase(it, model_.sharers.end());
    model_.sharers.push_back(s);
    model_.someone_sharing = !model_.sharers.empty();

    // Mark user as streaming in channel user list
    for (auto& ch : model_.channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(sharer_id)) u.streaming = true;

    model_.dirty("sharers");
    model_.dirty("someone_sharing");
    model_.dirty("channels");
}

void AppCore::on_screen_share_stopped(const uint8_t* data, size_t len)
{
    if (len < 4) return;
    uint32_t sharer_id;
    std::memcpy(&sharer_id, data, 4);

    auto it = std::remove_if(model_.sharers.begin(), model_.sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    model_.sharers.erase(it, model_.sharers.end());
    model_.someone_sharing = !model_.sharers.empty();

    // Clear streaming flag on user
    for (auto& ch : model_.channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(sharer_id)) u.streaming = false;

    model_.dirty("sharers");
    model_.dirty("someone_sharing");
    model_.dirty("channels");

    if (viewing_sharer_ == sharer_id)
        stop_watching();
}

void AppCore::on_screen_share_denied(const uint8_t* /*data*/, size_t /*len*/)
{
    model_.is_sharing = false;
    model_.dirty("is_sharing");
    if (bridge_.stop_screen_share) bridge_.stop_screen_share();
    LOG_WARN("Screen share denied by server");
}

void AppCore::on_admin_result(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint8_t ok      = reader.read_u8();
    std::string msg = reader.read_string();
    if (!msg.empty()) {
        model_.admin_message = Rml::String(msg);
        model_.dirty("admin_message");
    }
    (void)ok;
}

void AppCore::on_server_error(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    std::string msg = reader.read_string();
    if (reader.error()) return;

    LOG_ERROR("Server error: {}", msg);

    if (server_model_.show_login) {
        server_model_.login_error  = Rml::String(msg);
        server_model_.login_status = "";
        server_model_.dirty("login_error");
        server_model_.dirty("login_status");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Speaking state (200 ms hysteresis)
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::update_speaking_state()
{
    if (!model_.is_connected || current_channel_ == 0) return;

    auto now    = std::chrono::steady_clock::now();
    auto levels = mixer_.get_user_levels();
    bool changed = false;

    bool self_active = !model_.is_muted && audio_.voice_level() > 0.001f;
    if (self_active) voice_last_active_[user_id_] = now;

    for (auto& ch : model_.channels) {
        for (auto& user : ch.users) {
            bool was_speaking = user.speaking;
            UserId uid = static_cast<UserId>(user.id);

            bool active_now;
            if (uid == user_id_) {
                active_now = self_active;
            } else {
                auto it = levels.find(uid);
                active_now = (it != levels.end() && it->second > 0.001f);
            }
            if (user.muted || user.deafened) active_now = false;

            if (active_now) {
                voice_last_active_[uid] = now;
                user.speaking = true;
            } else {
                auto last_it = voice_last_active_.find(uid);
                if (last_it != voice_last_active_.end()) {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_it->second).count();
                    user.speaking = (ms < 200);
                } else {
                    user.speaking = false;
                }
            }
            if (user.speaking != was_speaking) changed = true;
        }
    }
    if (changed) model_.dirty("channels");
}

// ─────────────────────────────────────────────────────────────────────────────
// Preference helpers
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::apply_user_audio_prefs(UserId user_id)
{
    auto prefix = "user." + std::to_string(user_id);

    auto vol_str = settings_.get_pref(prefix + ".volume");
    if (vol_str) {
        float vol = std::strtof(vol_str->c_str(), nullptr);
        mixer_.set_user_volume(user_id, vol);
    }

    auto comp_str = settings_.get_pref(prefix + ".compress");
    if (comp_str) {
        bool enabled = (*comp_str == "1");
        float target = 0.8f;
        auto target_str = settings_.get_pref(prefix + ".compress_target");
        if (target_str) target = std::strtof(target_str->c_str(), nullptr);
        mixer_.set_user_compression(user_id, enabled, target);
    }
}

void AppCore::save_pref_debounced(const std::string& key, std::string value)
{
    pending_prefs_[key] = {std::move(value), std::chrono::steady_clock::now()};
}

void AppCore::flush_pending_prefs(bool force)
{
    if (pending_prefs_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    for (auto it = pending_prefs_.begin(); it != pending_prefs_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.updated).count();
        if (force || age >= 500) {
            settings_.set_pref(it->first, it->second.value);
            it = pending_prefs_.erase(it);
        } else {
            ++it;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Model callbacks
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::setup_model_callbacks()
{
    model_.on_disconnect_server = [this]() { net_.disconnect(); };
    model_.on_join_channel  = [this](int id) { join_channel(static_cast<ChannelId>(id)); };
    model_.on_leave_channel = [this]()       { leave_channel(); };

    model_.on_toggle_mute = [this]() {
        if (model_.ptt_enabled) return;
        bool muted = !audio_.is_muted();
        audio_.set_muted(muted);
        model_.is_muted = muted;
        model_.dirty("is_muted");
        for (auto& ch : model_.channels)
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(user_id_)) u.muted = muted;
        model_.dirty("channels");
        if (bridge_.play_sound)
            bridge_.play_sound(muted ? SoundPlayer::Effect::Mute : SoundPlayer::Effect::Unmute);
        send_voice_state();
    };

    model_.on_toggle_deafen = [this]() {
        bool deafened = !audio_.is_deafened();
        audio_.set_deafened(deafened);
        model_.is_deafened = deafened;
        model_.dirty("is_deafened");
        for (auto& ch : model_.channels)
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(user_id_)) u.deafened = deafened;
        model_.dirty("channels");
        if (bridge_.play_sound)
            bridge_.play_sound(deafened ? SoundPlayer::Effect::Deafen
                                        : SoundPlayer::Effect::Undeafen);
        send_voice_state();
    };

    model_.on_select_capture = [this](int index) {
        audio_.set_capture_device(index);
        auto devs = audio_.get_capture_devices();
        if (index >= 0 && index < static_cast<int>(devs.size()))
            settings_.set_pref("audio.capture_device", devs[index].name);
    };

    model_.on_select_playback = [this](int index) {
        audio_.set_playback_device(index);
        auto devs = audio_.get_playback_devices();
        if (index >= 0 && index < static_cast<int>(devs.size()))
            settings_.set_pref("audio.playback_device", devs[index].name);
    };

    model_.on_denoise_changed           = [this](bool e) { audio_.set_denoise_enabled(e); settings_.set_pref("audio.denoise", e?"1":"0"); };
    model_.on_normalize_changed         = [this](bool e) { audio_.set_normalize_enabled(e); settings_.set_pref("audio.normalize", e?"1":"0"); };
    model_.on_normalize_target_changed  = [this](float t) { audio_.set_normalize_target(t); save_pref_debounced("audio.normalize_target", std::to_string(t)); };
    model_.on_aec_changed               = [this](bool e) { audio_.set_aec_enabled(e); settings_.set_pref("audio.aec", e?"1":"0"); };
    model_.on_vad_changed               = [this](bool e) { audio_.set_vad_enabled(e); settings_.set_pref("audio.vad", e?"1":"0"); };
    model_.on_vad_threshold_changed     = [this](float t) { audio_.set_vad_threshold(t); save_pref_debounced("audio.vad_threshold", std::to_string(t)); };

    model_.on_toggle_ptt = [this]() {
        model_.ptt_enabled = !model_.ptt_enabled;
        model_.dirty("ptt_enabled");
        settings_.set_pref("audio.ptt", model_.ptt_enabled ? "1" : "0");
        if (model_.ptt_enabled) { audio_.set_muted(true); model_.is_muted = true; model_.dirty("is_muted"); }
    };
    model_.on_ptt_bind         = [this]() { model_.ptt_binding = true; model_.dirty("ptt_binding"); };
    model_.on_ptt_delay_changed = [this](float d) { save_pref_debounced("audio.ptt_delay", std::to_string(static_cast<int>(d))); };
    model_.on_mute_bind = [this]() {
        model_.mute_binding = true; model_.deafen_binding = false; model_.ptt_binding = false;
        model_.dirty("mute_binding"); model_.dirty("deafen_binding"); model_.dirty("ptt_binding");
    };
    model_.on_deafen_bind = [this]() {
        model_.deafen_binding = true; model_.mute_binding = false; model_.ptt_binding = false;
        model_.dirty("deafen_binding"); model_.dirty("mute_binding"); model_.dirty("ptt_binding");
    };

    model_.on_toggle_share = [this]() {
        if (model_.is_sharing) {
            if (bridge_.stop_screen_share) bridge_.stop_screen_share();
        } else {
            if (bridge_.open_share_picker) bridge_.open_share_picker();
        }
    };
    model_.on_cancel_share = [this]() {
        model_.show_share_picker = false;
        model_.dirty("show_share_picker");
    };

    model_.on_watch_sharer  = [this](int id) { watch_sharer(static_cast<UserId>(id)); };
    model_.on_select_sharer = [this](int id) { watch_sharer(static_cast<UserId>(id)); };
    model_.on_stop_watching = [this]()       { stop_watching(); };

    model_.on_stream_volume_changed = [this](float v) {
        stream_audio_player_.set_volume(v);
        save_pref_debounced("audio.stream_volume", std::to_string(v));
    };

    // Admin operations
    model_.on_create_channel = [this]() {
        if (!authenticated_) return;
        std::string name(model_.new_channel_name);
        if (name.empty()) return;
        BinaryWriter w; w.write_string(name); w.write_u32(0);
        net_.send_message(protocol::ControlMessageType::ADMIN_CREATE_CHANNEL,
                          w.data().data(), w.data().size());
        model_.show_create_channel = false;
        model_.dirty("show_create_channel");
    };

    model_.on_delete_channel = [this](int id) {
        if (!authenticated_) return;
        BinaryWriter w; w.write_u32(static_cast<uint32_t>(id));
        net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                          w.data().data(), w.data().size());
    };

    model_.on_show_user_menu = [this](int user_id, std::string name, int user_role) {
        if (!authenticated_ || static_cast<uint32_t>(user_id) == user_id_) return;
        apply_user_audio_prefs(static_cast<UserId>(user_id));
        model_.menu_user_id            = user_id;
        model_.menu_user_name          = name;
        model_.menu_user_role          = user_role;
        model_.menu_can_roles          = model_.can_manage_roles && role_ <= user_role;
        model_.menu_can_kick           = model_.can_kick && role_ <= user_role;
        model_.menu_user_volume        = mixer_.get_user_volume(static_cast<UserId>(user_id));
        model_.menu_user_compress      = mixer_.get_user_compression(static_cast<UserId>(user_id));
        model_.menu_user_compress_target = mixer_.get_user_compression_target(static_cast<UserId>(user_id));
        model_.show_user_menu          = true;
        model_.dirty("menu_user_id");        model_.dirty("menu_user_name");
        model_.dirty("menu_user_role");      model_.dirty("menu_can_roles");
        model_.dirty("menu_can_kick");       model_.dirty("menu_user_volume");
        model_.dirty("menu_user_compress");  model_.dirty("menu_user_compress_target");
        model_.dirty("show_user_menu");
    };

    model_.on_set_user_role = [this](int user_id, int new_role) {
        if (!authenticated_) return;
        BinaryWriter w;
        w.write_u32(static_cast<uint32_t>(user_id));
        w.write_u8(static_cast<uint8_t>(new_role));
        net_.send_message(protocol::ControlMessageType::ADMIN_SET_ROLE,
                          w.data().data(), w.data().size());
    };

    model_.on_kick_user = [this](int user_id) {
        if (!authenticated_) return;
        BinaryWriter w; w.write_u32(static_cast<uint32_t>(user_id));
        net_.send_message(protocol::ControlMessageType::ADMIN_KICK_USER,
                          w.data().data(), w.data().size());
    };

    model_.on_user_volume_changed = [this](int user_id, float vol) {
        mixer_.set_user_volume(static_cast<UserId>(user_id), vol);
        save_pref_debounced("user." + std::to_string(user_id) + ".volume", std::to_string(vol));
    };

    model_.on_user_compress_changed = [this](int user_id, bool enabled, float target) {
        mixer_.set_user_compression(static_cast<UserId>(user_id), enabled, target);
        auto p = "user." + std::to_string(user_id);
        save_pref_debounced(p + ".compress", enabled ? "1" : "0");
        save_pref_debounced(p + ".compress_target", std::to_string(target));
    };

    model_.on_show_channel_menu = [this](int channel_id, std::string name) {
        if (!authenticated_ || !model_.can_manage_channels) return;
        if (bridge_.show_channel_menu) bridge_.show_channel_menu(channel_id, name);
    };

    // Identity backup/import
    model_.on_show_seed_phrase = [this]() {
        if (!has_identity_) return;
        if (model_.show_seed_phrase) {
            model_.show_seed_phrase = false;
            model_.identity_seed_phrase = "";
            model_.dirty("show_seed_phrase");
            model_.dirty("identity_seed_phrase");
            return;
        }
        model_.identity_seed_phrase = Rml::String(seed_phrase_);
        model_.show_seed_phrase = true;
        model_.dirty("identity_seed_phrase");
        model_.dirty("show_seed_phrase");
    };

    model_.on_copy_seed_phrase = [this]() {
        if (bridge_.copy_to_clipboard)
            bridge_.copy_to_clipboard(std::string(model_.identity_seed_phrase));
    };

    model_.on_show_private_key = [this]() {
        if (!has_identity_) return;
        if (model_.show_private_key) {
            model_.show_private_key = false;
            model_.identity_private_key = "";
            model_.dirty("show_private_key");
            model_.dirty("identity_private_key");
            return;
        }
        model_.identity_private_key = Rml::String(parties::secret_key_to_hex(secret_key_));
        model_.show_private_key = true;
        model_.dirty("identity_private_key");
        model_.dirty("show_private_key");
    };

    model_.on_copy_private_key = [this]() {
        if (bridge_.copy_to_clipboard)
            bridge_.copy_to_clipboard(std::string(model_.identity_private_key));
    };

    model_.on_show_import = [this]() {
        model_.show_import_identity = true;
        model_.import_phrase = "";
        model_.import_error  = "";
        model_.dirty("show_import_identity");
        model_.dirty("import_phrase");
        model_.dirty("import_error");
    };

    model_.on_do_import = [this]() {
        std::string input(model_.import_phrase);
        SecretKey sk{}; PublicKey pk{}; std::string sp;

        if (input.size() == 64 && parties::secret_key_from_hex(input, sk)) {
            if (!parties::derive_pubkey(sk, pk)) {
                model_.import_error = "Failed to derive public key";
                model_.dirty("import_error"); return;
            }
        } else if (parties::validate_seed_phrase(input)) {
            if (!parties::derive_keypair(input, sk, pk)) {
                model_.import_error = "Failed to derive keypair";
                model_.dirty("import_error"); return;
            }
            sp = input;
        } else {
            model_.import_error = "Enter a 12-word seed phrase or 64-char hex private key.";
            model_.dirty("import_error"); return;
        }

        if (!settings_.save_identity(sp, sk, pk)) {
            model_.import_error = "Failed to save identity";
            model_.dirty("import_error"); return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = sp;
        server_model_.fingerprint   = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity  = true;
        server_model_.dirty("fingerprint"); server_model_.dirty("has_identity");

        model_.show_import_identity  = false; model_.import_phrase = ""; model_.import_error = "";
        model_.show_seed_phrase      = false; model_.identity_seed_phrase = "";
        model_.show_private_key      = false; model_.identity_private_key = "";
        model_.dirty("show_import_identity"); model_.dirty("import_phrase"); model_.dirty("import_error");
        model_.dirty("show_seed_phrase"); model_.dirty("identity_seed_phrase");
        model_.dirty("show_private_key"); model_.dirty("identity_private_key");

        LOG_INFO("Identity imported: {}",
                    parties::public_key_fingerprint(pk));
    };

    model_.on_cancel_import = [this]() {
        model_.show_import_identity = false;
        model_.import_phrase = "";
        model_.import_error  = "";
        model_.dirty("show_import_identity");
        model_.dirty("import_phrase");
        model_.dirty("import_error");
    };

    // on_select_share_target and on_start_native_share are platform-specific;
    // set by Windows / macOS platform code after init().
}

void AppCore::setup_server_model_callbacks()
{
    server_model_.on_connect_server = [this](int id) {
        if (authenticated_ && id == server_model_.connected_server_id) return;
        if (server_model_.show_login) return;
        if (!has_identity_) {
            server_model_.show_onboarding = true;
            server_model_.dirty("show_onboarding");
            return;
        }
        auto saved = settings_.get_saved_servers();
        for (auto& srv : saved) {
            if (srv.id == id) {
                connecting_server_id_ = id;
                server_host_ = srv.host;
                server_port_ = static_cast<uint16_t>(srv.port);
                server_model_.login_username = Rml::String(srv.last_username);
                server_model_.login_error    = "";
                server_model_.login_status   = Rml::String(
                    srv.name + " - " + srv.host + ":" + std::to_string(srv.port));
                server_model_.show_login = true;
                server_model_.dirty("login_username"); server_model_.dirty("login_error");
                server_model_.dirty("login_status");   server_model_.dirty("show_login");
                break;
            }
        }
    };

    server_model_.on_delete_server = [this](int id) {
        settings_.delete_server(id);
        refresh_server_list();
    };

    server_model_.on_save_server = [this]() {
        auto& host     = server_model_.edit_host;
        auto& port_str = server_model_.edit_port;
        if (host.empty() || port_str.empty()) {
            server_model_.edit_error = "Please fill in all fields";
            server_model_.dirty("edit_error"); return;
        }
        int port = std::atoi(port_str.c_str());
        if (port <= 0 || port > 65535) {
            server_model_.edit_error = "Invalid port number";
            server_model_.dirty("edit_error"); return;
        }
        std::string name = std::string(host) + ":" + std::string(port_str);
        settings_.save_server(name, std::string(host), port, "", "");
        server_model_.show_add_form = false;
        server_model_.dirty("show_add_form");
        refresh_server_list();
    };

    server_model_.on_do_connect = [this]() { do_connect(); };

    server_model_.on_cancel_login = [this]() {
        server_model_.show_login = false;
        server_model_.dirty("show_login");
        net_.disconnect();
        awaiting_connection_ = false;
    };

    server_model_.on_show_server_menu = [this](int id) {
        if (bridge_.show_server_menu) bridge_.show_server_menu(id);
    };

    server_model_.on_generate_identity = [this]() {
        std::string phrase = parties::generate_seed_phrase();
        server_model_.seed_phrase       = Rml::String(phrase);
        server_model_.show_onboarding   = true;
        server_model_.show_restore      = false;
        server_model_.show_key_import   = false;
        server_model_.dirty("seed_phrase");
        server_model_.dirty("show_onboarding");
        server_model_.dirty("show_restore");
        server_model_.dirty("show_key_import");
    };

    server_model_.on_save_identity = [this]() {
        std::string phrase(server_model_.seed_phrase);
        SecretKey sk{}; PublicKey pk{};
        if (!parties::derive_keypair(phrase, sk, pk)) {
            LOG_ERROR("Failed to derive keypair"); return;
        }
        if (!settings_.save_identity(phrase, sk, pk)) {
            LOG_ERROR("Failed to save identity"); return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = phrase;
        server_model_.fingerprint   = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity  = true;
        server_model_.show_onboarding = false;
        server_model_.dirty("fingerprint"); server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding");
        LOG_INFO("Identity saved: {}",
                    parties::public_key_fingerprint(pk));
    };

    server_model_.on_restore_identity = [this]() {
        std::string phrase(server_model_.restore_phrase);
        if (!parties::validate_seed_phrase(phrase)) {
            server_model_.login_error = "Invalid seed phrase";
            server_model_.dirty("login_error"); return;
        }
        SecretKey sk{}; PublicKey pk{};
        if (!parties::derive_keypair(phrase, sk, pk)) {
            server_model_.login_error = "Failed to derive keypair";
            server_model_.dirty("login_error"); return;
        }
        if (!settings_.save_identity(phrase, sk, pk)) {
            server_model_.login_error = "Failed to save identity";
            server_model_.dirty("login_error"); return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = phrase;
        server_model_.fingerprint     = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity    = true;
        server_model_.show_onboarding = false;
        server_model_.show_restore    = false;
        server_model_.login_error     = "";
        server_model_.dirty("fingerprint");  server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding"); server_model_.dirty("show_restore");
        server_model_.dirty("login_error");
    };

    server_model_.on_show_restore = [this]() {
        server_model_.show_restore    = true;
        server_model_.restore_phrase  = "";
        server_model_.login_error     = "";
        server_model_.dirty("show_restore");
        server_model_.dirty("restore_phrase");
        server_model_.dirty("login_error");
    };

    server_model_.on_show_key_import = [this]() {
        server_model_.show_key_import = true;
        server_model_.show_restore    = false;
        server_model_.import_key_hex  = "";
        server_model_.login_error     = "";
        server_model_.dirty("show_key_import"); server_model_.dirty("show_restore");
        server_model_.dirty("import_key_hex");  server_model_.dirty("login_error");
    };

    server_model_.on_import_key = [this]() {
        std::string hex(server_model_.import_key_hex);
        SecretKey sk{}; PublicKey pk{};
        if (!parties::secret_key_from_hex(hex, sk)) {
            server_model_.login_error = "Invalid private key. Must be 64 hex characters.";
            server_model_.dirty("login_error"); return;
        }
        if (!parties::derive_pubkey(sk, pk)) {
            server_model_.login_error = "Failed to derive public key";
            server_model_.dirty("login_error"); return;
        }
        if (!settings_.save_identity("", sk, pk)) {
            server_model_.login_error = "Failed to save identity";
            server_model_.dirty("login_error"); return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = "";
        server_model_.fingerprint     = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity    = true;
        server_model_.show_onboarding = false;
        server_model_.show_key_import = false;
        server_model_.login_error     = "";
        server_model_.dirty("fingerprint");    server_model_.dirty("has_identity");
        server_model_.dirty("show_onboarding"); server_model_.dirty("show_key_import");
        server_model_.dirty("login_error");
    };

    server_model_.on_copy_fingerprint = [this]() {
        if (bridge_.copy_to_clipboard)
            bridge_.copy_to_clipboard(std::string(server_model_.fingerprint));
    };

    server_model_.on_copy_seed = [this]() {
        if (bridge_.copy_to_clipboard)
            bridge_.copy_to_clipboard(std::string(server_model_.seed_phrase));
    };

    server_model_.on_tofu_accept = [this]() {
        if (!tofu_pending_) return;
        settings_.trust_fingerprint(server_host_, server_port_, tofu_pending_fingerprint_);
        settings_.delete_resumption_ticket(server_host_, server_port_);
        tofu_pending_ = false;
        server_model_.show_tofu_warning = false;
        server_model_.show_login        = true;
        server_model_.login_status      = "Authenticating...";
        server_model_.dirty("show_tofu_warning");
        server_model_.dirty("show_login");
        server_model_.dirty("login_status");
        send_auth_identity();
    };

    server_model_.on_tofu_reject = [this]() {
        tofu_pending_ = false;
        server_model_.show_tofu_warning = false;
        server_model_.show_login        = false;
        server_model_.dirty("show_tofu_warning");
        server_model_.dirty("show_login");
        net_.disconnect();
    };
}

} // namespace parties::client
