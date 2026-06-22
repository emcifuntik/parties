#include <client/app_core.h>
#ifdef _WIN32
#include <client/auto_updater.h>
#endif
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/server_query.h>
#include <parties/log.h>

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace parties::client {

namespace {
// Auto-reconnect backoff: 1s, 2s, 4s, 8s, 16s→capped at 15s. `attempts` is the
// number of failures so far. Parenthesized to dodge the Windows min() macro.
int reconnect_backoff_ms(int attempts) {
    const int shift = (std::min)(attempts, 5);
    return (std::min)(15000, 500 * (1 << shift));
}
} // namespace

AppCore::AppCore() = default;
AppCore::~AppCore() { stop_query_thread(); }

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
    audio_.set_aux_mixer(&aux_mixer_);
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

    // Secondary/auxiliary stream (VOICE2): same wire shape as voice, its own
    // sequence space. Not gated by mute (mute is for the mic) but still requires
    // being authenticated and in a channel.
    audio_.on_secondary_encoded_frame = [this](const uint8_t* data, size_t len) {
        if (!authenticated_ || current_channel_ == 0) return;
        uint16_t seq = voice2_seq_++;
        std::vector<uint8_t> pkt(1 + 2 + len);
        pkt[0] = protocol::VOICE2_PACKET_TYPE;
        std::memcpy(pkt.data() + 1, &seq, 2);
        std::memcpy(pkt.data() + 3, data, len);
        net_.send_data(pkt.data(), pkt.size());
    };

    // Wire net callbacks
    // on_disconnected fires on the MsQuic worker thread, but on_disconnect_cleanup
    // mutates the RmlUi data model — marshal it to the main thread (tick()).
    net_.on_disconnected = [this]() { disconnect_pending_.store(true, std::memory_order_release); };

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
        } else if (type == protocol::VOICE2_PACKET_TYPE) {
            // Secondary stream — identical layout to VOICE: [type(1)][sender_id(4)][seq(2)][opus(N)].
            // Routed into the aux mixer (no makeup/normalization, own volume).
            if (len < 8) return;
            uint32_t sender_id;
            std::memcpy(&sender_id, data + 1, 4);
            if (sender_id == user_id_) return;
            uint16_t seq;
            std::memcpy(&seq, data + 5, 2);
            aux_mixer_.push_packet(sender_id, seq, data + 7, len - 7);
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
            // When watching several shares in the grid the server forwards audio
            // for all of them, but StreamAudioPlayer is a single decoder/jitter
            // queue — feeding it two senders would corrupt decoding. So we play
            // only the focused (primary) stream's audio. Per-sharer screen-share
            // audio mixing would need a StreamAudioPlayer-per-sharer (TODO).
            if (sender_id == viewing_sharer_)
                stream_audio_player_.push_packet(data + 5, len - 5);
        } else if (type == protocol::VIDEO_CONTROL_TYPE) {
            if (len >= 2 && data[1] == protocol::VIDEO_CTL_PLI && bridge_.request_keyframe)
                bridge_.request_keyframe();
        }
    };

    // Fires on the MsQuic worker thread — don't touch sqlite here. Stash the
    // bytes; tick() persists them on the main thread.
    net_.on_resumption_ticket = [this](const uint8_t* ticket, size_t len) {
        std::lock_guard<std::mutex> lock(pending_ticket_mutex_);
        pending_resumption_ticket_.assign(ticket, ticket + len);
        pending_ticket_dirty_ = true;
    };

    // Apply saved per-user prefs whenever the mixer creates a new stream
    mixer_.on_stream_created = [this](UserId uid) { apply_user_audio_prefs(uid); };

    // Setup model callbacks
    setup_model_callbacks();
    setup_chat_model_callbacks();
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
    if (!chat_model_.init(rml_context)) {
        LOG_ERROR("chat_model init failed");
        return false;
    }

    // Initialize audio
    audio_.init();
    stream_audio_player_.init();

#ifdef _WIN32
    updater_ = std::make_unique<AutoUpdater>();
    updater_->start();
#endif

    start_query_thread();

    return true;
}

void AppCore::shutdown()
{
#ifdef _WIN32
    if (updater_) updater_->stop();
    updater_.reset();
#endif
    stop_query_thread();
    intentional_disconnect_ = true;
    cancel_reconnect();
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
    if (disconnect_pending_.exchange(false, std::memory_order_acquire))
        on_disconnect_cleanup();

    // Persist a resumption ticket received on the QUIC worker thread.
    {
        std::vector<uint8_t> ticket;
        {
            std::lock_guard<std::mutex> lock(pending_ticket_mutex_);
            if (pending_ticket_dirty_) {
                ticket.swap(pending_resumption_ticket_);
                pending_ticket_dirty_ = false;
            }
        }
        if (!ticket.empty())
            settings_.save_resumption_ticket(server_host_, server_port_,
                                             ticket.data(), ticket.size());
    }

    // Server-list status polling runs only while the lobby is visible.
    lobby_visible_.store(!authenticated_, std::memory_order_relaxed);
    apply_query_results();

    if (awaiting_connection_) poll_connecting();

    // Auto-reconnect: fire the next attempt once the backoff timer elapses and
    // we aren't already mid-connect.
    if (reconnecting_ && !awaiting_connection_ && !authenticated_ &&
        std::chrono::steady_clock::now() >= reconnect_next_attempt_) {
        attempt_reconnect();
    }

    process_server_messages();
    update_speaking_state();
    flush_pending_prefs();

#ifdef _WIN32
    if (updater_ && updater_->poll()) {
        auto s = updater_->state();
        model_.update_available  = (s == AutoUpdater::State::UpdateAvailable
                                 || s == AutoUpdater::State::Downloading
                                 || s == AutoUpdater::State::ReadyToInstall);
        model_.update_downloading = (s == AutoUpdater::State::Downloading);
        model_.update_ready       = (s == AutoUpdater::State::ReadyToInstall);
        model_.update_version     = Rml::String("v") + updater_->latest_version().c_str();
    }
#endif

    // Process completed file downloads on main thread
    {
        std::vector<CompletedDownload> downloads;
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            downloads.swap(completed_downloads_);
        }
        for (auto& dl : downloads) {
            std::string file_name = "download";
            for (auto& msg : chat_model_.messages.get()) {
                for (auto& att : msg.attachments) {
                    if (att.id == static_cast<int64_t>(dl.attachment_id)) {
                        file_name = std::string(att.file_name);
                        break;
                    }
                }
            }
#ifdef _WIN32
            char save_buf[MAX_PATH] = {};
            strncpy(save_buf, file_name.c_str(), MAX_PATH - 1);
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFile = save_buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = "All Files\0*.*\0";
            ofn.Flags = OFN_OVERWRITEPROMPT;

            if (GetSaveFileNameA(&ofn)) {
                std::ofstream out(save_buf, std::ios::binary);
                if (out)
                    out.write(reinterpret_cast<const char*>(dl.data.data()), dl.data.size());
            }
#endif
        }
    }

    // Send keepalive ping every 2 seconds while connected
    if (authenticated_) {
        auto now = std::chrono::steady_clock::now();
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - ping_last_send_).count();
        if (since_last >= 2000) {
            ping_sent_at_ = now;
            ping_pending_ = true;
            ping_last_send_ = now;
            net_.send_message(protocol::ControlMessageType::KEEPALIVE_PING, nullptr, 0);
        }
    }

    // Stream FPS counter (updated once per second)
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - stream_fps_last_update_).count();
    if (elapsed >= 1.0f) {
        uint32_t sc = stream_frame_count_.exchange(0, std::memory_order_relaxed);
        int sfps = static_cast<int>(sc / elapsed);
        if (sfps != model_.stream_fps) {
            model_.stream_fps = sfps;
        }
        stream_fps_last_update_ = now;
    }

    // Update local mic level indicator
    if (authenticated_ && current_channel_ != 0) {
        float lvl = audio_.voice_level();
        if (std::abs(lvl - model_.voice_level) > 0.01f) {
            model_.voice_level = lvl;
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
        model_.capture_devices.silent().push_back({Rml::String(d.name), d.index});
    for (auto& d : playback_devs)
        model_.playback_devices.silent().push_back({Rml::String(d.name), d.index});
    model_.capture_devices.notify();
    model_.playback_devices.notify();

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

    v = pref("audio.voice_volume");
    if (!v.empty()) { float vol = std::strtof(v.c_str(), nullptr); mixer_.set_master_volume(vol); model_.voice_volume = vol; }

    v = pref("audio.secondary_volume");
    if (!v.empty()) { float vol = std::strtof(v.c_str(), nullptr); aux_mixer_.set_master_volume(vol); model_.secondary_volume = vol; }

    v = pref("audio.notification_volume");
    if (!v.empty()) { float vol = std::strtof(v.c_str(), nullptr); if (bridge_.set_notification_volume) bridge_.set_notification_volume(vol); model_.notification_volume = vol; }

    v = pref("video.share_bitrate");
    if (!v.empty()) model_.share_bitrate = std::strtof(v.c_str(), nullptr);

    v = pref("video.share_fps");
    if (!v.empty()) model_.share_fps = std::atoi(v.c_str());

    v = pref("video.share_codec");
    if (!v.empty()) model_.share_codec = std::atoi(v.c_str());

    v = pref("video.share_scale");
    if (!v.empty()) model_.share_scale = std::atoi(v.c_str());

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
    if (identity && !identity->seed_phrase.empty()) {
        seed_phrase_ = identity->seed_phrase;
        secret_key_  = identity->secret_key;
        public_key_  = identity->public_key;
        has_identity_ = true;
    } else {
        // No saved identity (or legacy identity without seed phrase) —
        // show onboarding modal so the user can generate/restore.
        if (identity) {
            // Legacy identity with no seed phrase — discard it so the user
            // gets a fresh identity with a proper seed phrase backup.
            settings_.delete_identity();
        }
        has_identity_ = false;
        server_model_.show_onboarding = true;
    }

    if (!username_hint.empty())
        username_ = username_hint;

    server_model_.has_identity = has_identity_;
    server_model_.fingerprint  = Rml::String(settings_.get_fingerprint());
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
    auto& servers = server_model_.servers.silent();
    servers.clear();
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
        servers.push_back(std::move(entry));
    }
    size_t n = servers.size();
    server_model_.party_count_text = Rml::String(
        std::to_string(n) + (n == 1 ? " party" : " parties"));
    server_model_.servers.notify();

    // Publish targets for the background polling thread and request a re-merge
    // of any last-known status onto the freshly rebuilt entries.
    {
        std::lock_guard<std::mutex> lock(query_targets_mutex_);
        query_targets_.clear();
        for (auto& s : saved)
            query_targets_.push_back({ s.id, s.host, static_cast<uint16_t>(s.port) });
    }
    query_results_dirty_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server-list query polling
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::start_query_thread()
{
    if (query_thread_run_.exchange(true)) return;  // already running
    query_thread_ = std::thread([this] { query_thread_loop(); });
}

void AppCore::stop_query_thread()
{
    if (!query_thread_run_.exchange(false)) return;  // not running
    if (query_thread_.joinable()) query_thread_.join();
}

void AppCore::query_thread_loop()
{
    using namespace std::chrono;
    while (query_thread_run_.load(std::memory_order_relaxed)) {
        if (lobby_visible_.load(std::memory_order_relaxed)) {
            std::vector<QueryTarget> targets;
            {
                std::lock_guard<std::mutex> lock(query_targets_mutex_);
                targets = query_targets_;
            }
            for (auto& t : targets) {
                if (!query_thread_run_.load(std::memory_order_relaxed)) break;
                auto info = parties::query_server(t.host, t.port, 800);
                {
                    std::lock_guard<std::mutex> lock(query_results_mutex_);
                    QueryResult& r = query_results_[t.id];
                    if (info) {
                        r.online = true;
                        r.cur    = info->current_users;
                        r.max    = info->max_users;
                        r.locked = info->password_locked;
                        r.name   = info->server_name;
                    } else {
                        r.online = false;
                    }
                }
                query_results_dirty_.store(true, std::memory_order_release);
            }
        }
        // Refresh roughly every 3s, waking promptly on shutdown.
        for (int i = 0; i < 30 && query_thread_run_.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(milliseconds(100));
    }
}

void AppCore::apply_query_results()
{
    if (!query_results_dirty_.exchange(false, std::memory_order_acq_rel)) return;

    std::lock_guard<std::mutex> lock(query_results_mutex_);
    auto& servers = server_model_.servers.silent();
    bool changed = false;
    for (auto& entry : servers) {
        auto it = query_results_.find(entry.id);
        if (it == query_results_.end()) continue;
        const QueryResult& r = it->second;
        Rml::String users = r.online
            ? Rml::String(std::to_string(r.cur) + " / " + std::to_string(r.max))
            : Rml::String();

        // Keep the saved name until the query gives us the server's real name;
        // once online with a non-empty name, switch the display to it (and the
        // derived initials/gradient). Falls back to the saved name when offline.
        Rml::String name     = entry.name;
        Rml::String initials = entry.initials;
        if (r.online && !r.name.empty()) {
            name = Rml::String(r.name);
            if (r.name.size() >= 2)      initials = Rml::String(r.name.substr(0, 2));
            else if (!r.name.empty())    initials = Rml::String(r.name.substr(0, 1));
        }

        if (entry.online != r.online || entry.locked != r.locked ||
            entry.users_text != users || entry.name != name) {
            entry.online     = r.online;
            entry.locked     = r.locked;
            entry.users_text = std::move(users);
            entry.name       = std::move(name);
            entry.initials   = std::move(initials);
            changed = true;
        }
    }
    if (changed) server_model_.servers.notify();
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection flow
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::do_connect()
{
    if (!has_identity_) {
        server_model_.login_error = "No identity — generate seed phrase first";
        return;
    }

    // Manual connect supersedes any in-flight auto-reconnect.
    cancel_reconnect();

    username_ = server_model_.login_username;
    server_password_ = std::string(server_model_.login_password);
    server_model_.login_error = "";

    if (net_.is_connected()) { finish_connect(); return; }
    if (net_.is_connecting()) return;

    server_model_.login_status = "Connecting...";

    auto ticket = settings_.load_resumption_ticket(server_host_, server_port_);
    if (!net_.connect(server_host_, server_port_,
                      ticket.empty() ? nullptr : ticket.data(), ticket.size())) {
        server_model_.login_error = "Failed to connect to server";
        return;
    }
    awaiting_connection_ = true;
}

void AppCore::poll_connecting()
{
    if (net_.connect_failed()) {
        awaiting_connection_ = false;
        net_.disconnect();
        if (reconnecting_) {
            // Stay silent in the lobby banner; schedule the next attempt.
            reconnect_attempts_++;
            const int delay_ms = reconnect_backoff_ms(reconnect_attempts_);
            reconnect_next_attempt_ = std::chrono::steady_clock::now()
                                    + std::chrono::milliseconds(delay_ms);
            server_model_.reconnect_status = Rml::String("Reconnecting… (attempt ")
                + std::to_string(reconnect_attempts_ + 1).c_str() + ")";
        } else {
            server_model_.login_error  = "Failed to connect to server";
            server_model_.login_status = "";
        }
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

    // On TLS 1.3 session resumption (0-RTT / PSK), no certificate is exchanged,
    // so the fingerprint is empty. The session ticket is scoped to the server
    // that issued it, so a successful resumption confirms identity. We already
    // have the fingerprint stored in the TOFU database from the initial handshake.
    if (fp.empty()) {
        // PSK resumption — server already trusted (ticket wouldn't exist otherwise)
    } else {
        auto result = settings_.check_fingerprint(server_host_, server_port_, fp);

        if (result == Settings::TofuResult::Mismatch) {
            tofu_pending_fingerprint_ = fp;
            tofu_pending_ = true;
            server_model_.tofu_fingerprint  = Rml::String(fp);
            server_model_.show_tofu_warning = true;
            server_model_.show_login        = false;
            return;
        }
        if (result == Settings::TofuResult::Unknown)
            settings_.trust_fingerprint(server_host_, server_port_, fp);
    }

    server_model_.login_status = "Authenticating...";
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
        return;
    }

    BinaryWriter writer;
    writer.write_u16(protocol::PROTOCOL_VERSION);
    writer.write_bytes(public_key_.data(), public_key_.size());
    writer.write_string(username_);
    writer.write_u64(now);
    writer.write_bytes(sig.data(), sig.size());
    // Use the cached server password (set by do_connect on a fresh login and by
    // attempt_reconnect from saved settings) — the UI field is cleared/empty
    // during an auto-reconnect, so we can't read it here.
    writer.write_string(server_password_);

    net_.send_message(protocol::ControlMessageType::AUTH_IDENTITY,
                      writer.data().data(), writer.data().size());

    // Clear password from the UI field after sending
    server_model_.login_password = "";
}

void AppCore::on_disconnect_cleanup()
{
    // Capture pre-reset state needed to decide whether (and how) to reconnect.
    const bool was_authenticated = authenticated_;
    const ChannelId prev_channel = current_channel_;

    authenticated_ = false;
    current_channel_ = 0;
    server_password_.clear();
    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    video_frame_number_ = 0;
    awaiting_connection_ = false;
    awaiting_channel_join_ = false;
    pending_channel_id_ = 0;
    // Per-connection counters/timers — reset so a reconnect starts clean
    // rather than resuming sequence numbers / pending pings from the dead link.
    voice_seq_ = 0;
    ping_pending_ = false;
    tofu_pending_ = false;

    audio_.stop();
    mixer_.clear();
    aux_mixer_.clear();
    clear_all_sharers();
    voice_last_active_.clear();

    model_.is_connected = false;
    if (bridge_.play_sound) bridge_.play_sound(SoundPlayer::Effect::ServerDisconnected);
    model_.ping_ms = 0;
    ping_pending_ = false;
    model_.current_channel = 0;
    model_.current_channel_name = "";
    model_.channels.silent().clear();
    model_.is_muted = false;
    model_.is_deafened = false;
    model_.is_sharing = false;
    model_.show_settings = false;
    model_.show_share_picker = false;
    model_.show_create_channel = false;
    model_.my_role = 3;
    model_.can_manage_channels = false;
    chat_model_.can_manage_channels = false;
    model_.can_kick = false;
    model_.can_manage_roles = false;
    model_.admin_message = "";
    model_.dirty_all();

    // Clear chat state
    model_.show_chat = false;
    pending_uploads_.clear();
    {
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        completed_downloads_.clear();
    }
    chat_model_.text_channels.silent().clear();
    chat_model_.messages.silent().clear();
    chat_model_.active_channel = 0;
    chat_model_.active_channel_name = "";
    chat_model_.has_more_history = false;
    chat_model_.show_search = false;
    chat_model_.show_pinned = false;
    chat_model_.compose_text = "";
    chat_model_.text_channels.notify();
    chat_model_.messages.notify();

    server_model_.connected_server_id = 0;
    server_model_.show_login          = false;
    server_model_.show_add_form       = false;
    server_model_.login_error         = "";
    server_model_.login_status        = "";

    // ── Decide whether to auto-reconnect ────────────────────────────────────
    if (intentional_disconnect_) {
        // User/explicit teardown — do not reconnect.
        cancel_reconnect();
        intentional_disconnect_ = false;
    } else if (was_authenticated && connecting_server_id_ != 0) {
        // Unexpected drop while connected. Guard against reconnect loops: if a
        // freshly re-authenticated session drops again almost immediately
        // (e.g. a duplicate-login kicking us straight back out), count it as a
        // "flap" and give up after a few rather than ping-ponging forever.
        const auto session_dur = std::chrono::steady_clock::now() - auth_success_time_;
        if (session_dur < std::chrono::seconds(5)) {
            if (++reconnect_flaps_ >= 4) {
                LOG_WARN("Auto-reconnect: session keeps dropping; giving up");
                cancel_reconnect();
                reconnect_flaps_ = 0;
                server_model_.login_error = "Disconnected (are you signed in elsewhere?)";
                intentional_disconnect_ = false;
                return;
            }
        } else {
            reconnect_flaps_ = 0;
        }
        reconnecting_           = true;
        reconnect_attempts_     = 0;
        reconnect_channel_      = prev_channel;
        rejoin_pending_         = false;
        reconnect_next_attempt_ = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(500);
        server_model_.reconnecting     = true;
        server_model_.reconnect_status = "Connection lost — reconnecting…";
        LOG_WARN("Connection lost; auto-reconnect armed (channel {})", prev_channel);
    } else if (reconnecting_) {
        // A reconnect attempt failed before authentication completed.
        // Schedule the next attempt with capped exponential backoff.
        reconnect_attempts_++;
        const int delay_ms = reconnect_backoff_ms(reconnect_attempts_);
        reconnect_next_attempt_ = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(delay_ms);
        server_model_.reconnecting     = true;
        server_model_.reconnect_status = Rml::String("Reconnecting… (attempt ")
            + std::to_string(reconnect_attempts_ + 1).c_str() + ")";
        LOG_INFO("Reconnect attempt {} failed; retrying in {} ms",
                 reconnect_attempts_, delay_ms);
    }
}

void AppCore::disconnect_intentionally()
{
    intentional_disconnect_ = true;
    cancel_reconnect();
    net_.disconnect();
}

void AppCore::cancel_reconnect()
{
    reconnecting_       = false;
    rejoin_pending_     = false;
    reconnect_attempts_ = 0;
    reconnect_channel_  = 0;
    server_model_.reconnecting     = false;
    server_model_.reconnect_status = "";
}

void AppCore::attempt_reconnect()
{
    // Reload saved credentials for the server we were connected to. They
    // persist in settings across the disconnect cleanup.
    const SavedServer* match = nullptr;
    auto saved = settings_.get_saved_servers();
    for (auto& srv : saved) {
        if (srv.id == connecting_server_id_) { match = &srv; break; }
    }
    if (!match) {
        LOG_WARN("Auto-reconnect: saved server {} gone; giving up", connecting_server_id_);
        cancel_reconnect();
        return;
    }

    username_        = match->last_username;
    server_password_ = match->password;
    server_host_     = match->host;
    server_port_     = static_cast<uint16_t>(match->port);

    LOG_INFO("Auto-reconnect: dialing {}:{} (attempt {})",
             server_host_, server_port_, reconnect_attempts_ + 1);

    auto ticket = settings_.load_resumption_ticket(server_host_, server_port_);
    if (!net_.connect(server_host_, server_port_,
                      ticket.empty() ? nullptr : ticket.data(), ticket.size())) {
        // connect() failed synchronously — schedule the next attempt.
        reconnect_attempts_++;
        const int delay_ms = reconnect_backoff_ms(reconnect_attempts_);
        reconnect_next_attempt_ = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(delay_ms);
        server_model_.reconnect_status = Rml::String("Reconnecting… (attempt ")
            + std::to_string(reconnect_attempts_ + 1).c_str() + ")";
        return;
    }

    awaiting_connection_ = true;
    rejoin_pending_      = (reconnect_channel_ != 0);
    server_model_.reconnect_status = "Reconnecting…";
}

// ─────────────────────────────────────────────────────────────────────────────
// Channel operations
// ─────────────────────────────────────────────────────────────────────────────

void AppCore::join_channel(ChannelId id)
{
    if (!authenticated_) return;

    // Always dismiss chat view when clicking a voice channel
    if (model_.show_chat) {
        chat_model_.active_channel = 0;
        chat_model_.active_channel_name = "";
        model_.show_chat = false;
    }

    if (id == current_channel_) return;

    // Deselect text channel when joining voice channel
    chat_model_.active_channel = 0;
    chat_model_.active_channel_name = "";

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
    }
    if (model_.is_sharing && bridge_.stop_screen_share)
        bridge_.stop_screen_share();

    if (viewing_sharer_ != 0)
        stop_watching();
    clear_all_sharers();
    model_.is_sharing = false;

    net_.send_message(protocol::ControlMessageType::CHANNEL_LEAVE, nullptr, 0);

    auto& channels = model_.channels.silent();
    for (auto& ch : channels) {
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

    if (bridge_.play_sound)
        bridge_.play_sound(SoundPlayer::Effect::LeaveChannel);
    audio_.stop();
    mixer_.clear();
    aux_mixer_.clear();

    model_.current_channel = 0;
    model_.current_channel_name = "";
    model_.channels.notify();
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen share helpers
// ─────────────────────────────────────────────────────────────────────────────

// Single-select watch (replace whatever we were watching with just `id`). Used
// by single-decoder platforms (macOS/iOS) and as the "focus" action. On a
// multi-stream platform a sharer card / chip instead routes through add_watch.
void AppCore::watch_sharer(UserId id)
{
    if (id == 0) { stop_watching(); return; }
    if (multi_stream()) { add_watch(id); return; }

    // Tear down the previous single stream (server sub + decoder), keep at most one.
    if (viewing_sharer_ != 0 && viewing_sharer_ != id && bridge_.stop_video_stream)
        bridge_.stop_video_stream(viewing_sharer_);
    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        watched_.clear();
        watched_.insert(id);
    }
    viewing_sharer_ = id;
    awaiting_keyframe_ = true;
    if (bridge_.start_video_stream) bridge_.start_video_stream(id);

    uint32_t id32 = id;   // legacy single-select wire form ([target] only)
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      reinterpret_cast<const uint8_t*>(&id32), 4);
    send_pli(id);
    model_.viewing_sharer_id = static_cast<int>(id);
    rebuild_watched_model();
}

// Add one more stream to the grid (additive on multi-stream platforms; falls
// back to single-select where only one hardware decoder exists).
void AppCore::add_watch(UserId id)
{
    if (id == 0) return;
    if (!multi_stream()) { watch_sharer(id); return; }

    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        if (!watched_.insert(id).second) return;   // already watching
    }
    BinaryWriter w; w.write_u32(id); w.write_u8(1);   // additive subscribe
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      w.data().data(), w.data().size());
    if (bridge_.start_video_stream) bridge_.start_video_stream(id);
    send_pli(id);
    viewing_sharer_ = id;   // most-recently-added is the "primary" (fullscreen focus)
    model_.viewing_sharer_id = static_cast<int>(id);
    rebuild_watched_model();
}

void AppCore::remove_watch(UserId id)
{
    bool was_watching, empty_now;
    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        was_watching = watched_.erase(id) > 0;
        empty_now = watched_.empty();
    }
    if (!was_watching) return;

    BinaryWriter w; w.write_u32(id); w.write_u8(0);   // additive unsubscribe
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      w.data().data(), w.data().size());
    if (bridge_.stop_video_stream) bridge_.stop_video_stream(id);

    if (empty_now) {
        viewing_sharer_ = 0;
        model_.viewing_sharer_id = 0;
        model_.stream_fullscreen = false;
        if (bridge_.clear_video_element) bridge_.clear_video_element();
    } else {
        UserId primary;
        { std::lock_guard<std::mutex> lock(watched_mutex_); primary = *watched_.begin(); }
        viewing_sharer_ = primary;
        model_.viewing_sharer_id = static_cast<int>(primary);
    }
    rebuild_watched_model();
}

void AppCore::toggle_watch(UserId id)
{
    if (is_watching(id)) remove_watch(id);
    else                 add_watch(id);
}

void AppCore::set_single_watched(UserId id)
{
    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        watched_.clear();
        if (id != 0) watched_.insert(id);
    }
    viewing_sharer_ = id;
    model_.viewing_sharer_id = static_cast<int>(id);
    if (id == 0) model_.stream_fullscreen = false;
    rebuild_watched_model();
}

bool AppCore::is_watching(UserId id) const
{
    std::lock_guard<std::mutex> lock(watched_mutex_);
    return watched_.count(id) > 0;
}

void AppCore::stop_watching()
{
    std::vector<UserId> to_stop;
    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        to_stop.assign(watched_.begin(), watched_.end());
        watched_.clear();
    }
    for (UserId id : to_stop)
        if (bridge_.stop_video_stream) bridge_.stop_video_stream(id);

    uint32_t zero = 0;   // target 0 = unsubscribe from everyone (both wire forms)
    net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_VIEW,
                      reinterpret_cast<const uint8_t*>(&zero), 4);
    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    model_.viewing_sharer_id = 0;
    // Leaving the stream view must also drop fullscreen, or the window stays
    // borderless-fullscreen with nothing to show (the app.cpp tick syncs the OS
    // window fullscreen to this flag).
    model_.stream_fullscreen = false;
    if (bridge_.clear_video_element)
        bridge_.clear_video_element();
    rebuild_watched_model();
}

// Refresh model_.watched (grid cells) + sharers[].watching flags from watched_.
void AppCore::rebuild_watched_model()
{
    std::unordered_set<UserId> w;
    { std::lock_guard<std::mutex> lock(watched_mutex_); w = watched_; }

    auto& sharers = model_.sharers.silent();
    auto& wv = model_.watched.silent();
    wv.clear();
    for (auto& s : sharers) {
        bool watching = w.count(static_cast<UserId>(s.id)) > 0;
        s.watching = watching;
        if (watching) {
            WatchedStream ws;
            ws.id = s.id;
            ws.name = s.name;
            ws.element_id = Rml::String("screen-share-" + std::to_string(s.id));
            wv.push_back(ws);
        }
    }
    model_.sharers.notify();
    model_.watched.notify();
    model_.watching_count = static_cast<int>(wv.size());
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
    // Tear down any active decoders (channel left / disconnected). No server
    // unsubscribe is sent — the channel/connection is already gone.
    std::vector<UserId> to_stop;
    {
        std::lock_guard<std::mutex> lock(watched_mutex_);
        to_stop.assign(watched_.begin(), watched_.end());
        watched_.clear();
    }
    for (UserId id : to_stop)
        if (bridge_.stop_video_stream) bridge_.stop_video_stream(id);

    viewing_sharer_ = 0;
    awaiting_keyframe_ = false;
    active_sharers_.clear();
    model_.sharers.silent().clear();
    model_.sharers.notify();
    model_.watched.silent().clear();
    model_.watched.notify();
    model_.watching_count = 0;
    model_.someone_sharing = false;
    model_.viewing_sharer_id = 0;
    model_.stream_fullscreen = false;
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
    case protocol::ControlMessageType::CHAT_CHANNEL_LIST:
        on_chat_channel_list(data, len); break;
    case protocol::ControlMessageType::CHAT_COMMAND_LIST:
        on_chat_command_list(data, len); break;
    case protocol::ControlMessageType::CHAT_COMMAND_INPUT_LIST:
        on_chat_command_input_list(data, len); break;
    case protocol::ControlMessageType::CHAT_COMMAND_QUERY_RESP:
        on_chat_command_query_resp(data, len); break;
    case protocol::ControlMessageType::CHAT_MESSAGE:
        on_chat_message(data, len); break;
    case protocol::ControlMessageType::CHAT_HISTORY_RESP:
        on_chat_history_resp(data, len); break;
    case protocol::ControlMessageType::CHAT_MESSAGE_DELETED:
        on_chat_message_deleted(data, len); break;
    case protocol::ControlMessageType::CHAT_SEARCH_RESP:
        on_chat_search_resp(data, len); break;
    case protocol::ControlMessageType::CHAT_PINNED_RESP:
        on_chat_pinned_resp(data, len); break;
    case protocol::ControlMessageType::CHAT_FILE_READY:
        on_chat_file_ready(data, len); break;
    case protocol::ControlMessageType::KEEPALIVE_PONG:
        if (ping_pending_) {
            ping_pending_ = false;
            auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ping_sent_at_).count();
            model_.ping_ms = static_cast<int>(rtt);
        }
        break;
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
    auth_success_time_ = std::chrono::steady_clock::now();

    settings_.save_server(server_name, server_host_, server_port_,
                          net_.get_server_fingerprint(), username_, server_password_);
    server_password_.clear();
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
    if (bridge_.play_sound) bridge_.play_sound(SoundPlayer::Effect::ServerConnected);
    model_.my_role              = role_;
    model_.can_manage_channels  = (role_ <= static_cast<int>(parties::Role::Moderator));
    chat_model_.can_manage_channels = model_.can_manage_channels;
    model_.can_kick             = (role_ <= static_cast<int>(parties::Role::Moderator));
    model_.can_manage_roles     = (role_ <= static_cast<int>(parties::Role::Admin));

    server_model_.connected_server_id = connecting_server_id_;
    server_model_.show_login          = false;
    server_model_.login_error         = "";
    server_model_.login_status        = "";

    // Populate audio device lists
    auto caps = audio_.get_capture_devices();
    auto& cap_devices = model_.capture_devices.silent();
    cap_devices.clear();
    for (auto& d : caps)
        cap_devices.push_back({Rml::String(d.name), d.index});
    auto plays = audio_.get_playback_devices();
    auto& play_devices = model_.playback_devices.silent();
    play_devices.clear();
    for (auto& d : plays)
        play_devices.push_back({Rml::String(d.name), d.index});
    model_.capture_devices.notify();
    model_.playback_devices.notify();

    // ── Auto-reconnect: rejoin the last voice channel and clear the banner ──
    if (rejoin_pending_) {
        rejoin_pending_ = false;
        const ChannelId ch = reconnect_channel_;
        cancel_reconnect();
        if (ch != 0) {
            LOG_INFO("Auto-reconnect: rejoining channel {}", ch);
            join_channel(ch);
        }
    } else {
        // A fresh, manual login — make sure no stale reconnect state lingers.
        cancel_reconnect();
    }
}

void AppCore::on_channel_list(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    auto& channels = model_.channels.silent();
    std::unordered_map<int, Rml::Vector<ChannelUser>> old_users;
    for (auto& ch : channels)
        old_users[ch.id] = std::move(ch.users);

    channels.clear();
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
        channels.push_back(std::move(ch));
    }
    model_.channels.notify();
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

    auto& channels = model_.channels.silent();
    for (auto& ch : channels) {
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
            for (auto& ch : channels) {
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

        for (auto& ch : channels) {
            if (ch.id == static_cast<int>(channel_id)) {
                model_.current_channel_name = ch.name;
                for (auto& u : ch.users)
                    if (static_cast<uint32_t>(u.id) != user_id_)
                        apply_user_audio_prefs(static_cast<UserId>(u.id));
                break;
            }
        }
        audio_.start();
        if (bridge_.play_sound)
            bridge_.play_sound(SoundPlayer::Effect::JoinChannel);
    }
    model_.channels.notify();
}

void AppCore::on_user_joined(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    std::string uname   = reader.read_string();
    uint32_t channel_id = reader.read_u32();
    uint8_t urole       = reader.has_remaining(1) ? reader.read_u8() : 3;
    if (reader.error()) return;

    auto& channels = model_.channels.silent();
    for (auto& ch : channels) {
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
    model_.channels.notify();
}

void AppCore::on_user_left(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid        = reader.read_u32();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    mixer_.remove_user(uid);
    aux_mixer_.remove_user(uid);

    auto& channels = model_.channels.silent();
    for (auto& ch : channels) {
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
    model_.channels.notify();
}

void AppCore::on_user_voice_state(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t uid   = reader.read_u32();
    uint8_t muted  = reader.read_u8();
    uint8_t deaf   = reader.read_u8();
    if (reader.error()) return;

    auto& channels = model_.channels.silent();
    for (auto& ch : channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(uid)) {
                u.muted    = (muted != 0);
                u.deafened = (deaf  != 0);
            }
    model_.channels.notify();
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
        chat_model_.can_manage_channels = model_.can_manage_channels;
        model_.can_kick            = (new_role <= static_cast<int>(parties::Role::Moderator));
        model_.can_manage_roles    = (new_role <= static_cast<int>(parties::Role::Admin));
    }
    auto& channels = model_.channels.silent();
    for (auto& ch : channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(uid))
                u.role = new_role;
    model_.channels.notify();
}

void AppCore::on_screen_share_started(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint32_t sharer_id = reader.read_u32();
    if (reader.error()) return;

    auto& channels = model_.channels.silent();
    std::string sharer_name = "Unknown";
    for (auto& ch : channels)
        if (ch.id == static_cast<int>(current_channel_))
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(sharer_id)) { sharer_name = u.name.c_str(); break; }

    ActiveSharer s;
    s.id   = static_cast<int>(sharer_id);
    s.name = Rml::String(sharer_name);

    auto& sharers = model_.sharers.silent();
    auto it = std::remove_if(sharers.begin(), sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    sharers.erase(it, sharers.end());
    sharers.push_back(s);
    model_.someone_sharing = !sharers.empty();

    // Mark user as streaming in channel user list
    for (auto& ch : channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(sharer_id)) u.streaming = true;

    model_.channels.notify();
    rebuild_watched_model();   // notifies sharers + refreshes watching flags
}

void AppCore::on_screen_share_stopped(const uint8_t* data, size_t len)
{
    if (len < 4) return;
    uint32_t sharer_id;
    std::memcpy(&sharer_id, data, 4);

    auto& sharers = model_.sharers.silent();
    auto it = std::remove_if(sharers.begin(), sharers.end(),
        [sharer_id](const ActiveSharer& a) { return a.id == static_cast<int>(sharer_id); });
    sharers.erase(it, sharers.end());
    model_.someone_sharing = !sharers.empty();

    // Clear streaming flag on user
    auto& channels = model_.channels.silent();
    for (auto& ch : channels)
        for (auto& u : ch.users)
            if (u.id == static_cast<int>(sharer_id)) u.streaming = false;

    model_.channels.notify();

    // If we were watching this sharer, tear that stream down; otherwise just
    // refresh the model so the (now-gone) sharer drops out of the grid/chips.
    if (is_watching(static_cast<UserId>(sharer_id)))
        remove_watch(static_cast<UserId>(sharer_id));
    else
        rebuild_watched_model();
}

void AppCore::on_screen_share_denied(const uint8_t* /*data*/, size_t /*len*/)
{
    model_.is_sharing = false;
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
    }
    (void)ok;
}

void AppCore::on_server_error(const uint8_t* data, size_t len)
{
    BinaryReader reader(data, len);
    uint16_t code_raw = reader.read_u16();
    std::string msg = reader.read_string();
    if (reader.error()) return;
    auto code = static_cast<protocol::ServerErrorCode>(code_raw);

    LOG_ERROR("Server error [{}]: {}", code_raw, msg);

    // Terminal, server-initiated disconnects (admin kick, or being replaced by
    // a login from elsewhere) arrive as an error immediately followed by a
    // transport drop. Mark the upcoming drop intentional so we don't fight it.
    if (authenticated_ &&
        (code == protocol::ServerErrorCode::Kicked ||
         code == protocol::ServerErrorCode::Replaced)) {
        intentional_disconnect_ = true;
        if (bridge_.play_sound)
            bridge_.play_sound(SoundPlayer::Effect::ServerDisconnected);
    }

    if (server_model_.show_login) {
        server_model_.login_error  = Rml::String(msg);
        server_model_.login_status = "";
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

    bool self_active = !model_.is_muted && audio_.is_transmitting();
    if (self_active) voice_last_active_[user_id_] = now;

    auto& channels = model_.channels.silent();
    for (auto& ch : channels) {
        for (auto& user : ch.users) {
            bool was_speaking = user.speaking;
            UserId uid = static_cast<UserId>(user.id);

            bool active_now;
            if (uid == user_id_) {
                active_now = self_active;
            } else {
                auto it = levels.find(uid);
                active_now = (it != levels.end() && it->second > 0.0f);
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
    if (changed) model_.channels.notify();
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
    model_.on_disconnect_server = [this]() { disconnect_intentionally(); };
    model_.on_join_channel  = [this](int id) { join_channel(static_cast<ChannelId>(id)); };
    model_.on_leave_channel = [this]()       { leave_channel(); };

    model_.on_toggle_mute = [this]() {
        bool muted = !model_.is_muted;
        model_.is_muted = muted;
        // Effective audio mute: manually muted, or deafened, or PTT idle
        audio_.set_muted(muted || model_.is_deafened);
        auto& channels = model_.channels.silent();
        for (auto& ch : channels)
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(user_id_)) u.muted = muted;
        model_.channels.notify();
        if (bridge_.play_sound)
            bridge_.play_sound(muted ? SoundPlayer::Effect::Mute : SoundPlayer::Effect::Unmute);
        send_voice_state();
    };

    model_.on_toggle_deafen = [this]() {
        bool deafened = !model_.is_deafened;
        audio_.set_deafened(deafened);
        model_.is_deafened = deafened;
        // Deafen implies mute at the audio level
        if (deafened) {
            audio_.set_muted(true);
        } else {
            // Restore: only unmute audio if not manually muted (and not PTT-idle)
            audio_.set_muted(model_.is_muted);
        }
        auto& channels = model_.channels.silent();
        for (auto& ch : channels)
            for (auto& u : ch.users)
                if (u.id == static_cast<int>(user_id_)) u.deafened = deafened;
        model_.channels.notify();
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
    model_.on_voice_volume_changed      = [this](float v) { mixer_.set_master_volume(v); save_pref_debounced("audio.voice_volume", std::to_string(v)); };
    model_.on_secondary_volume_changed  = [this](float v) { aux_mixer_.set_master_volume(v); save_pref_debounced("audio.secondary_volume", std::to_string(v)); };

    model_.on_toggle_ptt = [this]() {
        model_.ptt_enabled = !model_.ptt_enabled;
        settings_.set_pref("audio.ptt", model_.ptt_enabled ? "1" : "0");
        // PTT on: start with mic muted (key not held yet)
        // PTT off: restore audio mute to match manual mute state
        audio_.set_muted(model_.is_muted || model_.is_deafened || model_.ptt_enabled);
    };
    model_.on_ptt_bind         = [this]() { model_.ptt_binding = true; };
    model_.on_ptt_delay_changed = [this](float d) { save_pref_debounced("audio.ptt_delay", std::to_string(static_cast<int>(d))); };
    model_.on_mute_bind = [this]() {
        model_.mute_binding = true; model_.deafen_binding = false; model_.ptt_binding = false;
    };
    model_.on_deafen_bind = [this]() {
        model_.deafen_binding = true; model_.mute_binding = false; model_.ptt_binding = false;
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
    };

    model_.on_watch_sharer  = [this](int id) { add_watch(static_cast<UserId>(id)); };
    model_.on_toggle_watch  = [this](int id) { toggle_watch(static_cast<UserId>(id)); };
    model_.on_select_sharer = [this](int id) { watch_sharer(static_cast<UserId>(id)); };
    model_.on_stop_watching = [this]()       { stop_watching(); };

    model_.on_stream_volume_changed = [this](float v) {
        stream_audio_player_.set_volume(v);
        save_pref_debounced("audio.stream_volume", std::to_string(v));
    };

    model_.on_notification_volume_changed = [this](float v) {
        if (bridge_.set_notification_volume) bridge_.set_notification_volume(v);
        save_pref_debounced("audio.notification_volume", std::to_string(v));
    };

    model_.on_test_notification_sound = [this]() {
        if (bridge_.play_sound) bridge_.play_sound(SoundPlayer::Effect::UserJoined);
    };

#ifdef _WIN32
    model_.on_apply_update = [this]() {
        if (!updater_) return;
        auto s = updater_->state();
        if (s == AutoUpdater::State::UpdateAvailable) {
            updater_->download();
        } else if (s == AutoUpdater::State::ReadyToInstall) {
            updater_->apply_and_restart();
        }
    };
#endif

    // Admin operations
    model_.on_create_channel = [this]() {
        if (!authenticated_) return;
        std::string name(model_.new_channel_name);
        if (name.empty()) return;
        BinaryWriter w; w.write_string(name); w.write_u32(0);
        net_.send_message(protocol::ControlMessageType::ADMIN_CREATE_CHANNEL,
                          w.data().data(), w.data().size());
        model_.show_create_channel = false;
    };

    model_.on_delete_channel = [this](int id) {
        if (!authenticated_) return;
        BinaryWriter w; w.write_u32(static_cast<uint32_t>(id));
        net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                          w.data().data(), w.data().size());
    };

    model_.on_rename_channel = [this]() {
        if (!authenticated_) return;
        std::string new_name(model_.new_rename_channel_name);
        if (new_name.empty()) return;
        BinaryWriter w;
        w.write_u32(static_cast<uint32_t>(model_.rename_channel_id));
        w.write_string(new_name);
        net_.send_message(protocol::ControlMessageType::ADMIN_RENAME_CHANNEL,
                          w.data().data(), w.data().size());
        model_.show_rename_channel = false;
    };

    model_.on_show_user_menu = [this](int user_id, std::string name, int user_role) {
        if (!authenticated_ || static_cast<uint32_t>(user_id) == user_id_) return;
        apply_user_audio_prefs(static_cast<UserId>(user_id));
        model_.menu_user_id            = user_id;
        model_.menu_user_name          = name;
        model_.menu_user_role          = user_role;
        const bool is_bot = user_role == static_cast<int>(parties::Role::Bot);
        model_.menu_can_roles          = model_.can_manage_roles && role_ <= user_role && !is_bot;
        model_.menu_can_kick           = model_.can_kick && role_ <= user_role && !is_bot;
        model_.menu_user_volume        = mixer_.get_user_volume(static_cast<UserId>(user_id));
        model_.menu_user_compress      = mixer_.get_user_compression(static_cast<UserId>(user_id));
        model_.menu_user_compress_target = mixer_.get_user_compression_target(static_cast<UserId>(user_id));
        model_.show_user_menu          = true;
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
            return;
        }
        model_.identity_seed_phrase = Rml::String(seed_phrase_);
        model_.show_seed_phrase = true;
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
            return;
        }
        model_.identity_private_key = Rml::String(parties::secret_key_to_hex(secret_key_));
        model_.show_private_key = true;
    };

    model_.on_copy_private_key = [this]() {
        if (bridge_.copy_to_clipboard)
            bridge_.copy_to_clipboard(std::string(model_.identity_private_key));
    };

    model_.on_show_import = [this]() {
        model_.show_import_identity = true;
        model_.import_phrase = "";
        model_.import_error  = "";
    };

    model_.on_do_import = [this]() {
        std::string input(model_.import_phrase);
        SecretKey sk{}; PublicKey pk{}; std::string sp;

        if (input.size() == 64 && parties::secret_key_from_hex(input, sk)) {
            if (!parties::derive_pubkey(sk, pk)) {
                model_.import_error = "Failed to derive public key";
                return;
            }
        } else if (parties::validate_seed_phrase(input)) {
            if (!parties::derive_keypair(input, sk, pk)) {
                model_.import_error = "Failed to derive keypair";
                return;
            }
            sp = input;
        } else {
            model_.import_error = "Enter a 12-word seed phrase or 64-char hex private key.";
            return;
        }

        if (!settings_.save_identity(sp, sk, pk)) {
            model_.import_error = "Failed to save identity";
            return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = sp;
        server_model_.fingerprint   = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity  = true;

        model_.show_import_identity  = false; model_.import_phrase = ""; model_.import_error = "";
        model_.show_seed_phrase      = false; model_.identity_seed_phrase = "";
        model_.show_private_key      = false; model_.identity_private_key = "";

        LOG_INFO("Identity imported: {}",
                    parties::public_key_fingerprint(pk));
    };

    model_.on_cancel_import = [this]() {
        model_.show_import_identity = false;
        model_.import_phrase = "";
        model_.import_error  = "";
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
            return;
        }
        auto saved = settings_.get_saved_servers();
        for (auto& srv : saved) {
            if (srv.id == id) {
                connecting_server_id_ = id;
                server_host_ = srv.host;
                server_port_ = static_cast<uint16_t>(srv.port);
                server_model_.login_username = Rml::String(srv.last_username);
                server_model_.login_password = Rml::String(srv.password);
                server_model_.login_error    = "";
                server_model_.login_status   = Rml::String(
                    srv.name + " - " + srv.host + ":" + std::to_string(srv.port));
                server_model_.show_login = true;
                break;
            }
        }
    };

    server_model_.on_delete_server = [this](int id) {
        settings_.delete_server(id);
        refresh_server_list();
    };

    server_model_.on_save_server = [this]() {
        const Rml::String& host     = server_model_.edit_host;
        const Rml::String& port_str = server_model_.edit_port;
        if (host.empty() || port_str.empty()) {
            server_model_.edit_error = "Please fill in all fields";
            return;
        }
        int port = std::atoi(port_str.c_str());
        if (port <= 0 || port > 65535) {
            server_model_.edit_error = "Invalid port number";
            return;
        }
        std::string name = std::string(host) + ":" + std::string(port_str);
        settings_.save_server(name, std::string(host), port, "", "");
        server_model_.show_add_form = false;
        refresh_server_list();
    };

    server_model_.on_do_connect = [this]() { do_connect(); };

    server_model_.on_cancel_login = [this]() {
        server_model_.show_login = false;
        disconnect_intentionally();
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
        LOG_INFO("Identity saved: {}",
                    parties::public_key_fingerprint(pk));
    };

    server_model_.on_restore_identity = [this]() {
        std::string phrase(server_model_.restore_phrase);
        if (!parties::validate_seed_phrase(phrase)) {
            server_model_.login_error = "Invalid seed phrase";
            return;
        }
        SecretKey sk{}; PublicKey pk{};
        if (!parties::derive_keypair(phrase, sk, pk)) {
            server_model_.login_error = "Failed to derive keypair";
            return;
        }
        if (!settings_.save_identity(phrase, sk, pk)) {
            server_model_.login_error = "Failed to save identity";
            return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = phrase;
        server_model_.fingerprint     = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity    = true;
        server_model_.show_onboarding = false;
        server_model_.show_restore    = false;
        server_model_.login_error     = "";
    };

    server_model_.on_show_restore = [this]() {
        server_model_.show_restore    = true;
        server_model_.restore_phrase  = "";
        server_model_.login_error     = "";
    };

    server_model_.on_show_key_import = [this]() {
        server_model_.show_key_import = true;
        server_model_.show_restore    = false;
        server_model_.import_key_hex  = "";
        server_model_.login_error     = "";
    };

    server_model_.on_import_key = [this]() {
        std::string hex(server_model_.import_key_hex);
        SecretKey sk{}; PublicKey pk{};
        if (!parties::secret_key_from_hex(hex, sk)) {
            server_model_.login_error = "Invalid private key. Must be 64 hex characters.";
            return;
        }
        if (!parties::derive_pubkey(sk, pk)) {
            server_model_.login_error = "Failed to derive public key";
            return;
        }
        if (!settings_.save_identity("", sk, pk)) {
            server_model_.login_error = "Failed to save identity";
            return;
        }
        secret_key_ = sk; public_key_ = pk; has_identity_ = true; seed_phrase_ = "";
        server_model_.fingerprint     = Rml::String(parties::public_key_fingerprint(pk));
        server_model_.has_identity    = true;
        server_model_.show_onboarding = false;
        server_model_.show_key_import = false;
        server_model_.login_error     = "";
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
        send_auth_identity();
    };

    server_model_.on_tofu_reject = [this]() {
        tofu_pending_ = false;
        server_model_.show_tofu_warning = false;
        server_model_.show_login        = false;
        disconnect_intentionally();
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Chat model callbacks

void AppCore::setup_chat_model_callbacks()
{
    chat_model_.on_select_channel = [this](int channel_id) {
        chat_model_.active_channel = channel_id;
        // Find channel name
        auto& tchannels = chat_model_.text_channels.silent();
        for (auto& tc : tchannels) {
            if (tc.id == channel_id) {
                chat_model_.active_channel_name = tc.name;
                tc.has_unread = false;
                break;
            }
        }
        chat_model_.text_channels.notify();
        chat_model_.messages.silent().clear();
        chat_model_.messages.notify();
        chat_model_.has_more_history = false;
        chat_model_.show_search = false;
        chat_model_.show_pinned = false;

        // Show chat view (keep voice channel connection intact)
        model_.show_chat = true;

        // Request history
        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(channel_id));
        writer.write_u64(0); // latest
        writer.write_u16(30);
        net_.send_message(protocol::ControlMessageType::CHAT_HISTORY_REQ,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_send_message = [this]() {
        if (chat_model_.active_channel == 0) return;
        bool has_text = !chat_model_.compose_text.get().empty();
        bool has_files = !chat_model_.pending_files.get().empty();
        if (!has_text && !has_files) return;

        // Read pending files into memory before sending
        pending_uploads_.clear();
        for (auto& pf : chat_model_.pending_files.get()) {
            std::ifstream in(std::string(pf.path), std::ios::binary | std::ios::ate);
            if (!in) continue;
            auto sz = in.tellg();
            in.seekg(0);
            PendingUpload pu;
            pu.path = std::string(pf.name);
            pu.data.resize(static_cast<size_t>(sz));
            in.read(reinterpret_cast<char*>(pu.data.data()), sz);
            pending_uploads_.push_back(std::move(pu));
        }

        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(chat_model_.active_channel));
        writer.write_string(has_text ? std::string(chat_model_.compose_text) : std::string());
        writer.write_u8(static_cast<uint8_t>(pending_uploads_.size()));
        for (auto& pu : pending_uploads_) {
            writer.write_string(pu.path);  // file_name
            writer.write_u64(static_cast<uint64_t>(pu.data.size()));  // file_size
            writer.write_string("application/octet-stream");  // mime
        }
        net_.send_message(protocol::ControlMessageType::CHAT_SEND,
                         writer.data().data(), writer.data().size());

        chat_model_.compose_text = "";
        chat_model_.pending_files.silent().clear();
        chat_model_.pending_files.notify();
    };

    chat_model_.on_load_more_history = [this]() {
        if (chat_model_.messages.get().empty() || !chat_model_.has_more_history)
            return;

        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(chat_model_.active_channel));
        writer.write_u64(static_cast<uint64_t>(chat_model_.messages.get().front().id));
        writer.write_u16(30);
        net_.send_message(protocol::ControlMessageType::CHAT_HISTORY_REQ,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_pin_message = [this](int64_t msg_id) {
        BinaryWriter writer;
        writer.write_u64(static_cast<uint64_t>(msg_id));
        net_.send_message(protocol::ControlMessageType::CHAT_PIN,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_unpin_message = [this](int64_t msg_id) {
        BinaryWriter writer;
        writer.write_u64(static_cast<uint64_t>(msg_id));
        net_.send_message(protocol::ControlMessageType::CHAT_UNPIN,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_delete_message = [this](int64_t msg_id) {
        BinaryWriter writer;
        writer.write_u64(static_cast<uint64_t>(msg_id));
        net_.send_message(protocol::ControlMessageType::CHAT_DELETE,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_message_context_menu = [this](int64_t msg_id) {
        if (bridge_.show_message_menu)
            bridge_.show_message_menu(msg_id);
    };

    chat_model_.on_do_search = [this]() {
        if (chat_model_.search_query.get().empty() || chat_model_.active_channel == 0)
            return;

        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(chat_model_.active_channel));
        writer.write_string(std::string(chat_model_.search_query));
        writer.write_u64(0); // latest
        writer.write_u16(30);
        net_.send_message(protocol::ControlMessageType::CHAT_SEARCH,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_request_pinned = [this]() {
        if (chat_model_.active_channel == 0) return;

        BinaryWriter writer;
        writer.write_u32(static_cast<uint32_t>(chat_model_.active_channel));
        net_.send_message(protocol::ControlMessageType::CHAT_PINNED_REQ,
                         writer.data().data(), writer.data().size());
    };

    chat_model_.on_create_text_channel = [this]() {
        if (!authenticated_) return;
        std::string name(chat_model_.new_text_channel_name);
        if (name.empty()) {
            LOG_WARN("Text channel name is empty, ignoring create request");
            return;
        }

        BinaryWriter writer;
        writer.write_string(name);
        net_.send_message(protocol::ControlMessageType::ADMIN_CREATE_TEXT_CHANNEL,
                         writer.data().data(), writer.data().size());

        chat_model_.show_create_text_channel = false;
    };

    chat_model_.on_open_url = [](const std::string& url) {
        // Validate URL starts with http(s) to prevent command injection
        if (url.find("http://") != 0 && url.find("https://") != 0) return;
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        // Use fork+exec to avoid shell injection
        if (fork() == 0) { execlp("open", "open", url.c_str(), nullptr); _exit(1); }
#else
        if (fork() == 0) { execlp("xdg-open", "xdg-open", url.c_str(), nullptr); _exit(1); }
#endif
    };

    chat_model_.on_attach_file = [this]() {
#ifdef _WIN32
        char file_buf[4096] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = file_buf;
        ofn.nMaxFile = sizeof(file_buf);
        ofn.lpstrFilter = "All Files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

        if (GetOpenFileNameA(&ofn)) {
            namespace fs = std::filesystem;
            // Single file: file_buf is full path
            // Multi-select: file_buf is dir\0file1\0file2\0\0
            if (ofn.nFileOffset > 0 && file_buf[ofn.nFileOffset - 1] == '\0') {
                // Multi-select
                std::string dir(file_buf);
                char* p = file_buf + dir.size() + 1;
                while (*p) {
                    fs::path fpath = fs::path(dir) / p;
                    PendingFile pf;
                    pf.name = Rml::String(fpath.filename().string());
                    pf.path = Rml::String(fpath.string());
                    auto sz = fs::file_size(fpath);
                    if (sz < 1024)
                        pf.size_str = Rml::String(std::to_string(sz) + " B");
                    else if (sz < 1024 * 1024)
                        pf.size_str = Rml::String(std::format("{:.1f} KB", sz / 1024.0));
                    else
                        pf.size_str = Rml::String(std::format("{:.1f} MB", sz / (1024.0 * 1024.0)));
                    chat_model_.pending_files.silent().push_back(std::move(pf));
                    p += strlen(p) + 1;
                }
            } else {
                // Single file
                fs::path fpath(file_buf);
                PendingFile pf;
                pf.name = Rml::String(fpath.filename().string());
                pf.path = Rml::String(fpath.string());
                auto sz = fs::file_size(fpath);
                if (sz < 1024)
                    pf.size_str = Rml::String(std::to_string(sz) + " B");
                else if (sz < 1024 * 1024)
                    pf.size_str = Rml::String(std::format("{:.1f} KB", sz / 1024.0));
                else
                    pf.size_str = Rml::String(std::format("{:.1f} MB", sz / (1024.0 * 1024.0)));
                chat_model_.pending_files.silent().push_back(std::move(pf));
            }
            chat_model_.pending_files.notify();
        }
#endif
    };

    chat_model_.on_download_file = [this](int64_t file_id) {
        net_.download_file(static_cast<uint64_t>(file_id));
    };

    net_.on_file_downloaded = [this](uint64_t attachment_id, std::vector<uint8_t> data) {
        // Push to queue — processed on main thread in tick()
        std::lock_guard<std::mutex> lock(downloads_mutex_);
        completed_downloads_.push_back({attachment_id, std::move(data)});
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Chat protocol handlers

static Rml::String format_timestamp(uint64_t unix_ts) {
    auto t = static_cast<std::time_t>(unix_ts);
    auto* tm = std::localtime(&t);
    if (!tm) return "";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%I:%M %p", tm);
    // Remove leading zero from hour: "09:32 AM" -> "9:32 AM"
    if (buf[0] == '0')
        return Rml::String(buf + 1);
    return Rml::String(buf);
}

static int day_of_timestamp(uint64_t unix_ts) {
    auto t = static_cast<std::time_t>(unix_ts);
    auto* tm = std::localtime(&t);
    if (!tm) return -1;
    return tm->tm_year * 400 + tm->tm_yday;
}

static Rml::String format_date_label(uint64_t unix_ts) {
    auto now = std::time(nullptr);
    int today = day_of_timestamp(static_cast<uint64_t>(now));
    int msg_day = day_of_timestamp(unix_ts);
    if (msg_day == today) return "Today";
    if (msg_day == today - 1) return "Yesterday";
    auto t = static_cast<std::time_t>(unix_ts);
    auto* tm = std::localtime(&t);
    if (!tm) return "";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%B %d, %Y", tm);
    return Rml::String(buf);
}

static void assign_date_labels(Rml::Vector<ChatMessage>& msgs) {
    int prev_day = -1;
    for (auto& m : msgs) {
        int d = day_of_timestamp(m.raw_timestamp);
        if (d != prev_day) {
            m.date_label = format_date_label(m.raw_timestamp);
            prev_day = d;
        } else {
            m.date_label.clear();
        }
    }
}

static int name_color_index(const Rml::String& name) {
    uint32_t hash = 0;
    for (char c : name) hash = hash * 31 + static_cast<uint8_t>(c);
    return static_cast<int>(hash % 12);
}

static Rml::String make_initials(const Rml::String& name) {
    if (name.empty()) return "?";
    Rml::String result;
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    // Find second word start
    auto sp = name.find(' ');
    if (sp != Rml::String::npos && sp + 1 < name.size())
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(name[sp + 1])));
    else if (name.size() > 1)
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(name[1])));
    return result;
}

static Rml::String file_extension_upper(const Rml::String& filename) {
    auto dot = filename.rfind('.');
    if (dot == Rml::String::npos || dot + 1 >= filename.size()) return "";
    Rml::String ext = filename.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

static Rml::Vector<TextSegment> split_text_with_urls(const Rml::String& text) {
    Rml::Vector<TextSegment> segments;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t http_pos = text.find("http://", pos);
        size_t https_pos = text.find("https://", pos);
        size_t url_start = (std::min)(http_pos, https_pos);
        if (url_start == Rml::String::npos) {
            if (pos < text.size())
                segments.push_back({text.substr(pos), false});
            break;
        }
        if (url_start > pos)
            segments.push_back({text.substr(pos, url_start - pos), false});
        // Find URL end (whitespace or end of string)
        size_t url_end = url_start;
        while (url_end < text.size() && text[url_end] != ' ' && text[url_end] != '\n'
               && text[url_end] != '\r' && text[url_end] != '\t')
            url_end++;
        // Strip trailing punctuation that's likely not part of URL
        while (url_end > url_start && (text[url_end - 1] == '.' || text[url_end - 1] == ','
               || text[url_end - 1] == ')' || text[url_end - 1] == ';'))
            url_end--;
        segments.push_back({text.substr(url_start, url_end - url_start), true});
        pos = url_end;
    }
    if (segments.empty())
        segments.push_back({text, false});
    return segments;
}

static ChatMessage parse_chat_message(BinaryReader& reader, uint32_t my_user_id = 0) {
    ChatMessage msg;
    msg.id = static_cast<int64_t>(reader.read_u64());
    uint32_t channel_id = reader.read_u32();
    (void)channel_id;
    msg.sender_id = static_cast<int>(reader.read_u32());
    msg.sender_name = Rml::String(reader.read_string());
    msg.initials = make_initials(msg.sender_name);
    msg.is_own = (static_cast<uint32_t>(msg.sender_id) == my_user_id);
    uint64_t ts = reader.read_u64();
    msg.raw_timestamp = ts;
    msg.timestamp_str = format_timestamp(ts);
    msg.text = Rml::String(reader.read_string());
    msg.segments = split_text_with_urls(msg.text);
    msg.has_url = std::any_of(msg.segments.begin(), msg.segments.end(),
                              [](const TextSegment& s) { return s.is_url; });
    msg.pinned = reader.read_u8() != 0;
    msg.color_index = name_color_index(msg.sender_name);

    uint8_t att_count = reader.read_u8();
    for (uint8_t i = 0; i < att_count && !reader.error(); i++) {
        ChatAttachment att;
        att.id = static_cast<int64_t>(reader.read_u64());
        att.file_name = Rml::String(reader.read_string());
        att.file_ext = file_extension_upper(att.file_name);
        uint64_t fsize = reader.read_u64();

        // Format size
        if (fsize < 1024)
            att.size_str = Rml::String(std::to_string(fsize) + " B");
        else if (fsize < 1024 * 1024)
            att.size_str = Rml::String(std::format("{:.1f} KB", fsize / 1024.0));
        else
            att.size_str = Rml::String(std::format("{:.1f} MB", fsize / (1024.0 * 1024.0)));

        std::string mime = reader.read_string(); // mime_type
        (void)mime;
        att.uploaded = reader.read_u8() != 0;
        msg.attachments.push_back(std::move(att));
    }

    return msg;
}

void AppCore::request_chat_command_query(uint64_t request_id,
                                         ChannelId text_channel_id,
                                         const std::string& command_name,
                                         const std::string& argument_name,
                                         const std::string& query,
                                         uint16_t cursor_pos) {
    if (!authenticated_ || text_channel_id == 0 || command_name.empty() || argument_name.empty())
        return;

    BinaryWriter writer;
    writer.write_u32(static_cast<uint32_t>(text_channel_id));
    writer.write_u64(request_id);
    writer.write_string(command_name);
    writer.write_string(argument_name);
    writer.write_string(query);
    writer.write_u16(cursor_pos);
    net_.send_message(protocol::ControlMessageType::CHAT_COMMAND_QUERY,
                     writer.data().data(), writer.data().size());

    chat_model_.command_query_request_id = request_id;
}

void AppCore::on_chat_channel_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t count = reader.read_u32();
    if (reader.error()) return;

    auto& channels = chat_model_.text_channels.silent();
    channels.clear();
    for (uint32_t i = 0; i < count && !reader.error(); i++) {
        TextChannel tc;
        tc.id = static_cast<int>(reader.read_u32());
        tc.name = Rml::String(reader.read_string());
        reader.read_u32(); // sort_order
        channels.push_back(std::move(tc));
    }
    chat_model_.text_channels.notify();
}

void AppCore::on_chat_command_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint16_t count = reader.read_u16();
    if (reader.error()) return;

    auto& commands = chat_model_.commands.silent();
    commands.clear();
    for (uint16_t i = 0; i < count; ++i) {
        ChatCommandDefinition cmd;
        cmd.name = reader.read_string();
        cmd.description = reader.read_string();
        cmd.usage = reader.read_string();
        if (reader.error()) break;
        commands.push_back(std::move(cmd));
    }
    chat_model_.commands.notify();
}

void AppCore::on_chat_command_input_list(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint16_t command_count = reader.read_u16();
    if (reader.error()) return;

    auto& commands = chat_model_.commands.silent();
    for (uint16_t i = 0; i < command_count && !reader.error(); ++i) {
        std::string command_name = reader.read_string();
        uint16_t input_count = reader.read_u16();

        auto command = std::find_if(commands.begin(), commands.end(),
            [&](const ChatCommandDefinition& candidate) {
                return candidate.name == command_name;
            });

        Rml::Vector<ChatCommandDefinition::Input> inputs;
        for (uint16_t j = 0; j < input_count && !reader.error(); ++j) {
            ChatCommandDefinition::Input input;
            input.argument_name = Rml::String(reader.read_string());
            input.mode = static_cast<int>(reader.read_u8());
            input.min_chars = static_cast<int>(reader.read_u16());
            input.debounce_ms = static_cast<int>(reader.read_u16());
            input.max_results = static_cast<int>(reader.read_u16());
            input.placeholder = Rml::String(reader.read_string());
            inputs.push_back(std::move(input));
        }

        if (command != commands.end())
            command->inputs = std::move(inputs);
    }

    if (!reader.error())
        chat_model_.commands.notify();
}

void AppCore::on_chat_command_query_resp(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint64_t request_id = reader.read_u64();
    reader.read_string(); // command_name
    reader.read_string(); // argument_name
    uint8_t status = reader.read_u8();
    std::string message = reader.read_string();
    uint16_t result_count = reader.read_u16();
    if (reader.error()) return;

    if (request_id != chat_model_.command_query_request_id.get())
        return;

    auto& results = chat_model_.command_query_results.silent();
    results.clear();
    for (uint16_t i = 0; i < result_count && !reader.error(); ++i) {
        ChatCommandQueryResult result;
        result.id = Rml::String(reader.read_string());
        result.title = Rml::String(reader.read_string());
        result.subtitle = Rml::String(reader.read_string());
        result.value = Rml::String(reader.read_string());
        result.kind = Rml::String(reader.read_string());
        result.duration_ms = static_cast<int>(reader.read_u32());
        result.thumbnail_url = Rml::String(reader.read_string());
        results.push_back(std::move(result));
    }

    if (reader.error())
        return;

    chat_model_.command_query_status = static_cast<int>(status);
    chat_model_.command_query_message = Rml::String(message);
    chat_model_.command_query_results.notify();
}

void AppCore::on_chat_message(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    auto msg = parse_chat_message(reader, user_id_);
    if (reader.error()) return;

    // Read channel_id from the message for routing (it's at offset 8)
    BinaryReader peek(data, len);
    peek.read_u64(); // msg_id
    uint32_t channel_id = peek.read_u32();

    // If this is our message and we have pending uploads, upload file data now
    if (msg.sender_id == static_cast<int>(user_id_) && !pending_uploads_.empty()) {
        // Match pending uploads to attachment IDs by index
        size_t upload_count = (std::min)(pending_uploads_.size(), msg.attachments.size());
        for (size_t i = 0; i < upload_count; i++) {
            net_.upload_file(static_cast<uint64_t>(msg.attachments[i].id),
                            pending_uploads_[i].data.data(),
                            pending_uploads_[i].data.size());
        }
        pending_uploads_.clear();
    }

    if (static_cast<int>(channel_id) == chat_model_.active_channel) {
        auto& msgs = chat_model_.messages.silent();
        // Check if this is an update to an existing message (e.g., pin state change)
        bool found = false;
        for (auto& m : msgs) {
            if (m.id == msg.id) {
                m.pinned = msg.pinned;
                m.text = msg.text;
                found = true;
                break;
            }
        }
        if (!found)
            msgs.push_back(std::move(msg));
        // Keep at most 30 messages displayed
        if (msgs.size() > 30) {
            msgs.erase(
                msgs.begin(),
                msgs.begin() +
                    static_cast<int>(msgs.size() - 30));
            chat_model_.has_more_history = true;
        }
        assign_date_labels(msgs);
        chat_model_.messages.notify();
    } else {
        // Mark channel as unread
        auto& channels = chat_model_.text_channels.silent();
        for (auto& tc : channels) {
            if (tc.id == static_cast<int>(channel_id)) {
                tc.has_unread = true;
                break;
            }
        }
        chat_model_.text_channels.notify();
    }
}

void AppCore::on_chat_history_resp(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint32_t channel_id = reader.read_u32();
    uint8_t has_more = reader.read_u8();
    uint16_t count = reader.read_u16();
    if (reader.error()) return;

    if (static_cast<int>(channel_id) != chat_model_.active_channel)
        return;

    // Parse messages
    Rml::Vector<ChatMessage> batch;
    for (uint16_t i = 0; i < count && !reader.error(); i++)
        batch.push_back(parse_chat_message(reader, user_id_));

    auto& msgs = chat_model_.messages.silent();
    // Prepend to existing messages (batch is oldest-first from server)
    if (!batch.empty()) {
        batch.insert(batch.end(), msgs.begin(), msgs.end());
        msgs = std::move(batch);
    }

    chat_model_.has_more_history = has_more != 0;
    assign_date_labels(msgs);
    chat_model_.messages.notify();
}

void AppCore::on_chat_message_deleted(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint64_t message_id = reader.read_u64();
    uint32_t channel_id = reader.read_u32();
    if (reader.error()) return;

    if (static_cast<int>(channel_id) == chat_model_.active_channel) {
        auto& msgs = chat_model_.messages.silent();
        msgs.erase(std::remove_if(msgs.begin(), msgs.end(),
            [message_id](const ChatMessage& m) { return m.id == static_cast<int64_t>(message_id); }),
            msgs.end());
        assign_date_labels(msgs);
        assign_date_labels(msgs);
        chat_model_.messages.notify();
    }
}

void AppCore::on_chat_search_resp(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    reader.read_u32(); // channel_id
    uint16_t count = reader.read_u16();
    if (reader.error()) return;

    auto& results = chat_model_.search_results.silent();
    results.clear();
    for (uint16_t i = 0; i < count && !reader.error(); i++)
        results.push_back(parse_chat_message(reader, user_id_));
    chat_model_.search_results.notify();
}

void AppCore::on_chat_pinned_resp(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    reader.read_u32(); // channel_id
    uint16_t count = reader.read_u16();
    if (reader.error()) return;

    auto& pinned = chat_model_.pinned_messages.silent();
    pinned.clear();
    for (uint16_t i = 0; i < count && !reader.error(); i++)
        pinned.push_back(parse_chat_message(reader, user_id_));
    chat_model_.pinned_messages.notify();
}

void AppCore::on_chat_file_ready(const uint8_t* data, size_t len) {
    BinaryReader reader(data, len);
    uint64_t message_id = reader.read_u64();
    uint64_t attachment_id = reader.read_u64();
    if (reader.error()) return;

    // Update attachment status in displayed messages
    auto& msgs = chat_model_.messages.silent();
    for (auto& msg : msgs) {
        if (msg.id == static_cast<int64_t>(message_id)) {
            for (auto& att : msg.attachments) {
                if (att.id == static_cast<int64_t>(attachment_id)) {
                    att.uploaded = true;
                    break;
                }
            }
            break;
        }
    }
    chat_model_.messages.notify();
}

} // namespace parties::client
