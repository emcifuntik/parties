#include <server/server.h>
#include <parties/crypto.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/types.h>
#include <parties/permissions.h>
#include <parties/video_common.h>
#include <parties/profiler.h>

#include <parties/log.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

namespace parties::server {

// Monotonic microseconds for the policy helpers (RateEstimator, TokenBucket,
// Coalescer treat 0 as "never", which steady_clock never yields).
static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A sharer's recorded largest keyframe (backlog headroom) is replaced by the
// newest keyframe once it is older than this, so a one-off huge keyframe does
// not inflate every viewer's budget forever.
constexpr int64_t KEYFRAME_HEADROOM_TTL_US = 10'000'000;

Server::Server() = default;
Server::~Server() { stop(); }

bool Server::start(const Config& cfg) {
	ZoneScopedN("Server::start");
    config_ = cfg;

    // Open database
    if (!db_.open(config_.db_path)) {
        LOG_ERROR("Failed to open database");
        return false;
    }

    if (!config_.root_fingerprints.empty()) {
        LOG_INFO("{} root fingerprint(s) configured", config_.root_fingerprints.size());
    }

    // Generate self-signed cert if not present
    if (!std::filesystem::exists(config_.cert_file) ||
        !std::filesystem::exists(config_.key_file)) {
        LOG_INFO("Generating self-signed certificate...");
        if (!parties::generate_self_signed_cert(config_.server_name,
                                                 config_.cert_file,
                                                 config_.key_file)) {
            LOG_ERROR("Failed to generate certificate");
            return false;
        }
        LOG_INFO("Certificate written to {} / {}", config_.cert_file, config_.key_file);
    }

    // Info reported to connectionless server queries (server-browser style).
    quic_.set_server_info(config_.server_name,
                          static_cast<uint16_t>(config_.max_clients),
                          !config_.server_password.empty());

    // Start QUIC transport (unified control + data plane)
    if (!quic_.start(config_.listen_ip, config_.port,
                     static_cast<size_t>(config_.max_clients),
                     config_.cert_file, config_.key_file)) {
        return false;
    }

    // Queue completed frames with a per-session byte cap. The server loop
    // restores encode order before applying gates and forwarding to viewers.
    quic_.on_video_frame = [this](uint32_t session_id, uint8_t packet_type,
                                  const uint8_t* data, size_t len) {
        if (packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            enqueue_video_frame(session_id, data, len);
        } else {
            // Non-video packets (control, stream audio) still go through the queue
            DataPacket pkt;
            pkt.session_id = session_id;
            pkt.packet_type = packet_type;
            pkt.channel_id = 0;
            pkt.reliable = true;
            pkt.data.assign(data, data + len);
            quic_.data_incoming().push(std::move(pkt));
        }
    };

    // Ensure file storage directory exists
    try {
        std::filesystem::create_directories(config_.chat.file_storage_path);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create file storage directory '{}': {}",
                  config_.chat.file_storage_path, e.what());
        return false;
    }

    running_ = true;
    LOG_INFO("{} started successfully", config_.server_name);
    return true;
}

void Server::run() {
    auto last_retention_check = std::chrono::steady_clock::now();
    uint64_t last_send_failures_ = 0;

    while (running_) {
        ZoneScopedN("Server::run");
        process_control_messages();
        process_data_packets();
        process_video_frames();
        process_file_transfers();
        process_disconnects();

        // Periodic retention enforcement (every 60 seconds)
        auto now = std::chrono::steady_clock::now();
        if (now - last_retention_check > std::chrono::seconds(60)) {
            last_retention_check = now;

            // Surface silent reliable-send drops (backpressure / teardown races).
            uint64_t fails = quic_.reliable_send_failures();
            if (fails > last_send_failures_) {
                LOG_WARN("{} reliable send(s) failed in the last interval (total {})",
                         fails - last_send_failures_, fails);
                last_send_failures_ = fails;
            }

            if (config_.chat.message_retention_days > 0) {
                int purged = db_.purge_old_messages(config_.chat.message_retention_days);
                if (purged > 0)
                    LOG_INFO("Purged {} old messages", purged);
            }
            if (config_.chat.file_retention == "time" && config_.chat.file_retention_days > 0) {
                int purged = db_.purge_old_files(config_.chat.file_retention_days,
                                                  config_.chat.file_storage_path);
                if (purged > 0)
                    LOG_INFO("Purged {} expired files", purged);
            } else if (config_.chat.file_retention == "ring" &&
                       config_.chat.max_total_file_storage > 0) {
                int purged = db_.purge_oldest_files(config_.chat.max_total_file_storage,
                                                     config_.chat.file_storage_path);
                if (purged > 0)
                    LOG_INFO("Purged {} files (ring buffer)", purged);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Server::stop() {
    if (!running_) return;
    running_ = false;
    quic_.stop();
    db_.close();
    LOG_INFO("Server stopped");
}

void Server::process_control_messages() {
	ZoneScopedN("Server::process_control_messages");
    auto messages = quic_.incoming().drain();
    for (auto& msg : messages)
        handle_message(msg);
}

void Server::process_data_packets() {
	ZoneScopedN("Server::process_data_packets");
    auto packets = quic_.data_incoming().drain();
    for (auto& pkt : packets) {
        if (pkt.packet_type == protocol::VOICE_PACKET_TYPE ||
            pkt.packet_type == protocol::VOICE2_PACKET_TYPE) {
            auto session = quic_.get_session(pkt.session_id);
            if (!session || !session->authenticated || session->channel_id == 0)
                continue;

            // Forward: [<voice type>][sender_user_id(u32)][seq(2)][opus_data].
            // The SFU treats the primary (VOICE) and secondary (VOICE2) streams
            // identically — it never decodes; it echoes the same type byte so the
            // receiver routes each into the right mixer.
            std::vector<uint8_t> fwd;
            fwd.reserve(1 + 4 + pkt.data.size());
            fwd.push_back(pkt.packet_type);
            uint32_t uid = session->user_id;
            fwd.insert(fwd.end(), reinterpret_cast<uint8_t*>(&uid),
                       reinterpret_cast<uint8_t*>(&uid) + 4);
            fwd.insert(fwd.end(), pkt.data.begin(), pkt.data.end());

            auto all_sessions = quic_.get_sessions();
            std::vector<uint32_t> targets;
            for (auto& s : all_sessions) {
                if (s->id != pkt.session_id &&
                    s->authenticated &&
                    s->channel_id == session->channel_id &&
                    !s->deafened) {
                    targets.push_back(s->id);
                }
            }
            // Voice goes out as priority datagrams so it is never queued
            // behind video toward the same peer.
            if (!targets.empty())
                quic_.send_to_many(targets, fwd.data(), fwd.size(), true);
        }
        else if (pkt.packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            enqueue_video_frame(pkt.session_id, pkt.data.data(), pkt.data.size());
        }
        else if (pkt.packet_type == protocol::STREAM_AUDIO_PACKET_TYPE) {
            forward_stream_audio(pkt);
        }
        else if (pkt.packet_type == protocol::VIDEO_CONTROL_TYPE) {
            handle_video_control(pkt);
        }
    }
}

void Server::process_file_transfers() {
    namespace fs = std::filesystem;

    // Handle completed file uploads
    auto uploads = quic_.file_uploads().drain();
    for (auto& ev : uploads) {
        auto session = quic_.get_session(ev.session_id);
        if (!session || !session->authenticated) continue;

        auto att = db_.get_attachment(ev.attachment_id);
        if (!att) {
            LOG_WARN("Upload for unknown attachment {}", ev.attachment_id);
            continue;
        }
        if (att->uploaded) continue;  // already uploaded

        // Ownership check: only the author of the message that owns this
        // attachment may supply its bytes. Otherwise any authenticated user who
        // learns a pending attachment_id could fill it with arbitrary content.
        auto owner_msg = db_.get_message(att->message_id);
        if (!owner_msg || owner_msg->sender_id != session->user_id) {
            LOG_WARN("Upload for attachment {} rejected: session {} (user {}) is not the message author",
                     ev.attachment_id, ev.session_id, session->user_id);
            continue;
        }

        // Check file size limit
        if (static_cast<int64_t>(ev.data.size()) > config_.chat.max_file_size) {
            LOG_WARN("File too large ({} bytes) from session {}", ev.data.size(), ev.session_id);
            continue;
        }

        // Create directory and write file
        fs::path full_path = fs::path(config_.chat.file_storage_path) / att->disk_path;
        fs::create_directories(full_path.parent_path());

        std::ofstream out(full_path, std::ios::binary);
        if (!out) {
            LOG_ERROR("Failed to create file: {}", full_path.string());
            continue;
        }
        out.write(reinterpret_cast<const char*>(ev.data.data()), ev.data.size());
        out.close();

        db_.mark_attachment_uploaded(ev.attachment_id);
        LOG_INFO("File uploaded: {} ({} bytes)", att->file_name, ev.data.size());

        // Broadcast CHAT_FILE_READY to all authenticated clients
        BinaryWriter writer;
        writer.write_u64(att->message_id);
        writer.write_u64(ev.attachment_id);
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::CHAT_FILE_READY,
                             writer.data().data(), writer.data().size());
        }
    }

    // Handle file download requests
    auto downloads = quic_.file_downloads().drain();
    for (auto& req : downloads) {
        auto session = quic_.get_session(req.session_id);
        if (!session || !session->authenticated) continue;

        auto att = db_.get_attachment(req.attachment_id);
        if (!att || !att->uploaded) {
            LOG_WARN("Download request for unavailable attachment {}", req.attachment_id);
            continue;
        }

        fs::path full_path = fs::path(config_.chat.file_storage_path) / att->disk_path;
        std::ifstream in(full_path, std::ios::binary | std::ios::ate);
        if (!in) {
            LOG_ERROR("Failed to read file: {}", full_path.string());
            continue;
        }

        auto fpos = in.tellg();
        size_t file_size = static_cast<size_t>(fpos);
        in.seekg(0);
        std::vector<uint8_t> file_data(file_size);
        in.read(reinterpret_cast<char*>(file_data.data()), static_cast<std::streamsize>(file_size));
        in.close();

        quic_.send_file_on_stream(req.stream, file_data.data(), file_data.size());
        LOG_INFO("File sent: {} ({} bytes)", att->file_name, file_size);
    }
}

void Server::send_error(uint32_t session_id, const std::string& message,
                        protocol::ServerErrorCode code) {
    BinaryWriter writer;
    writer.write_u16(static_cast<uint16_t>(code));
    writer.write_string(message);
    quic_.send_to(session_id, protocol::ControlMessageType::SERVER_ERROR,
                 writer.data().data(), writer.data().size());
}

void Server::send_channel_list(uint32_t session_id) {
	ZoneScopedN("Server::send_channel_list");
    auto channels = db_.get_all_channels();

    BinaryWriter writer;
    writer.write_u32(static_cast<uint32_t>(channels.size()));
    for (auto& ch : channels) {
        writer.write_u32(ch.id);
        writer.write_string(ch.name);
        writer.write_u32(static_cast<uint32_t>(ch.max_users));
        writer.write_u32(static_cast<uint32_t>(ch.sort_order));

        // Count users currently in this channel
        uint32_t user_count = 0;
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == ch.id)
                user_count++;
        }
        writer.write_u32(user_count);
    }

    quic_.send_to(session_id, protocol::ControlMessageType::CHANNEL_LIST,
                 writer.data().data(), writer.data().size());
}

void Server::send_text_channel_list(uint32_t session_id) {
    auto channels = db_.get_all_text_channels();

    BinaryWriter writer;
    writer.write_u32(static_cast<uint32_t>(channels.size()));
    for (auto& ch : channels) {
        writer.write_u32(ch.id);
        writer.write_string(ch.name);
        writer.write_u32(static_cast<uint32_t>(ch.sort_order));
    }

    quic_.send_to(session_id, protocol::ControlMessageType::CHAT_CHANNEL_LIST,
                 writer.data().data(), writer.data().size());
}

void Server::handle_message(const IncomingMessage& msg) {
	ZoneScopedN("Server::handle_message");
    auto session = quic_.get_session(msg.session_id);
    if (!session) return;

    switch (msg.type) {

    // ── Authentication (Ed25519 identity) ──────────────────────────────
    case protocol::ControlMessageType::AUTH_IDENTITY: {
        if (session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());

        // Check protocol version — reject only on a MAJOR mismatch so minor
        // (backwards-compatible) bumps don't lock out older clients.
        uint16_t client_version = reader.read_u16();
        if (reader.error()) {
            send_error(msg.session_id, "Malformed auth message",
                       protocol::ServerErrorCode::BadAuth);
            break;
        }
        if (protocol::protocol_major(client_version) != protocol::PROTOCOL_VERSION_MAJOR) {
            LOG_WARN("Protocol major mismatch: server={}, client={}",
                     protocol::PROTOCOL_VERSION_MAJOR, protocol::protocol_major(client_version));
            send_error(msg.session_id, std::format("Incompatible protocol version (server major {}, client major {})",
                protocol::PROTOCOL_VERSION_MAJOR, protocol::protocol_major(client_version)),
                protocol::ServerErrorCode::BadVersion);
            break;
        }

        // Read public key (32 bytes)
        PublicKey pubkey{};
        reader.read_bytes(pubkey.data(), 32);
        std::string display_name = reader.read_string();
        uint64_t timestamp = reader.read_u64();
        Signature sig{};
        reader.read_bytes(sig.data(), 64);
        if (reader.error()) {
            send_error(msg.session_id, "Malformed auth message",
                       protocol::ServerErrorCode::BadAuth);
            break;
        }

        // Verify timestamp freshness (±30s window).
        constexpr int64_t kAuthWindowSec = 30;
        auto now = static_cast<uint64_t>(std::time(nullptr));
        int64_t diff = static_cast<int64_t>(now) - static_cast<int64_t>(timestamp);
        if (diff > kAuthWindowSec || diff < -kAuthWindowSec) {
            send_error(msg.session_id, "Auth timestamp out of range",
                       protocol::ServerErrorCode::BadAuth);
            break;
        }

        Fingerprint fp = parties::public_key_fingerprint(pubkey);

        // Replay guard: a captured AUTH_IDENTITY reuses its (pubkey, timestamp)
        // pair and Ed25519 signature. Require a strictly newer timestamp than
        // the last one we accepted for this identity — legit re-auths use the
        // current time, replays reuse the stale one. Prune stale entries so the
        // map stays bounded by the active identity count.
        {
            for (auto it = recent_auth_.begin(); it != recent_auth_.end(); ) {
                if (static_cast<int64_t>(now) - static_cast<int64_t>(it->second) > kAuthWindowSec)
                    it = recent_auth_.erase(it);
                else
                    ++it;
            }
            auto seen = recent_auth_.find(fp);
            if (seen != recent_auth_.end() && timestamp <= seen->second) {
                LOG_WARN("Replayed/stale auth from fp={} (ts={}, last={})", fp, timestamp, seen->second);
                send_error(msg.session_id, "Stale authentication; please retry",
                           protocol::ServerErrorCode::BadAuth);
                break;
            }
        }

        // Reconstruct signed message: pubkey(32) + display_name + timestamp(8)
        BinaryWriter sig_msg;
        sig_msg.write_bytes(pubkey.data(), 32);
        sig_msg.write_string(display_name);
        sig_msg.write_u64(timestamp);

        // Verify Ed25519 signature
        if (!parties::ed25519_verify(sig_msg.data().data(), sig_msg.data().size(),
                                      sig, pubkey)) {
            send_error(msg.session_id, "Invalid signature",
                       protocol::ServerErrorCode::BadAuth);
            break;
        }

        // Read optional password (appended after signature)
        std::string client_password;
        if (!reader.error() && reader.remaining() > 0)
            client_password = reader.read_string();

        // Verify server password if configured (constant-time compare)
        if (!config_.server_password.empty()) {
            if (!parties::constant_time_equals(client_password, config_.server_password)) {
                send_error(msg.session_id, "Incorrect server password",
                           protocol::ServerErrorCode::BadPassword);
                break;
            }
        }

        // Accept: record the timestamp for the replay guard.
        recent_auth_[fp] = timestamp;
        LOG_INFO("Auth from session {}: name='{}' fp={}", msg.session_id, display_name, fp);

        // Look up or auto-create user
        auto user = db_.get_user_by_pubkey(pubkey);
        if (!user) {
            // Auto-create new user
            Role initial_role = Role::User;

            // Check if this fingerprint is a root user
            for (const auto& root_fp : config_.root_fingerprints) {
                if (root_fp == fp) {
                    initial_role = Role::Owner;
                    LOG_INFO("Root fingerprint matched -- granting Owner role");
                    break;
                }
            }

            if (!db_.create_user(pubkey, display_name, fp, initial_role)) {
                send_error(msg.session_id, "Failed to create user");
                break;
            }
            user = db_.get_user_by_pubkey(pubkey);
            if (!user) {
                send_error(msg.session_id, "Internal error");
                break;
            }
            LOG_INFO("New identity registered: '{}' (id={})", display_name, user->id);
        } else {
            // Update display name if changed
            if (user->display_name != display_name) {
                db_.update_display_name(user->id, display_name);
                user->display_name = display_name;
            }

            // Check root fingerprint — promote if needed
            for (const auto& root_fp : config_.root_fingerprints) {
                if (root_fp == fp && user->role != static_cast<int>(Role::Owner)) {
                    db_.set_user_role(user->id, Role::Owner);
                    user->role = static_cast<int>(Role::Owner);
                    LOG_INFO("Promoted user '{}' to Owner (root fingerprint)", display_name);
                    break;
                }
            }
        }

        // Kick any existing sessions with the same identity
        {
            auto all = quic_.get_sessions();
            for (auto& s : all) {
                if (s->id != msg.session_id && s->authenticated &&
                    s->public_key == pubkey) {
                    LOG_INFO("Kicking duplicate session {} for user '{}' (id={})", s->id, s->username, s->user_id);
                    // Tell the old session it was replaced so its client doesn't
                    // try to auto-reconnect and ping-pong with the new session.
                    send_error(s->id, "Signed in from another location",
                               protocol::ServerErrorCode::Replaced);
                    // Broadcast its departure now; the QUIC SHUTDOWN_COMPLETE
                    // path is idempotent (channel_id is captured there, and we
                    // clear it below so it won't double-broadcast).
                    process_disconnect(s->id, s->user_id, s->channel_id);
                    s->channel_id = 0;
                    s->authenticated = false;
                    quic_.disconnect(s->id);
                }
            }
        }

        // Auth success
        session->authenticated = true;
        session->user_id = user->id;
        session->username = user->display_name;
        session->role = user->role;
        session->public_key = pubkey;
        // Decides the video delivery path (per-frame streams for >= 1.2) and
        // which notifications this client understands.
        session->protocol_version = client_version;

        // user_id -> session index (the duplicate, if any, was erased by
        // process_disconnect above).
        {
            std::lock_guard<std::mutex> lock(user_index_mutex_);
            user_sessions_[session->user_id] = msg.session_id;
        }
        authed_sessions_[msg.session_id] = session;

        db_.update_last_login(user->id);

        // Generate session token
        parties::random_bytes(session->session_token.data(), session->session_token.size());

        // Send AUTH_RESPONSE: [user_id][session_token(32)][role][server_name][protocol_version(2)]
        // The trailing version (1.2+) tells the client which video path to use.
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_bytes(session->session_token.data(), session->session_token.size());
        writer.write_u8(static_cast<uint8_t>(session->role));
        writer.write_string(config_.server_name);
        writer.write_u16(protocol::PROTOCOL_VERSION);

        quic_.send_to(msg.session_id, protocol::ControlMessageType::AUTH_RESPONSE,
                     writer.data().data(), writer.data().size());

        // Send channel list immediately after auth
        send_channel_list(msg.session_id);
        send_text_channel_list(msg.session_id);

        // Send user lists for all channels so the client can see who's online
        {
            auto channels = db_.get_all_channels();
            auto all_sessions = quic_.get_sessions();
            for (auto& ch : channels) {
                BinaryWriter list_writer;
                list_writer.write_u32(ch.id);
                uint32_t count = 0;
                for (auto& s : all_sessions) {
                    if (s->authenticated && s->channel_id == ch.id)
                        count++;
                }
                list_writer.write_u32(count);
                for (auto& s : all_sessions) {
                    if (s->authenticated && s->channel_id == ch.id) {
                        list_writer.write_u32(s->user_id);
                        list_writer.write_string(s->username);
                        list_writer.write_u8(static_cast<uint8_t>(s->role));
                        list_writer.write_u8(s->muted ? 1 : 0);
                        list_writer.write_u8(s->deafened ? 1 : 0);
                    }
                }
                if (count > 0) {
                    quic_.send_to(msg.session_id, protocol::ControlMessageType::CHANNEL_USER_LIST,
                                 list_writer.data().data(), list_writer.data().size());
                }
            }
        }

        LOG_INFO("User '{}' (id={}, role={}) authenticated", user->display_name, user->id, user->role);
        break;
    }

    // ── Keepalive ───────────────────────────────────────────────────────
    case protocol::ControlMessageType::KEEPALIVE_PING: {
        quic_.send_to(msg.session_id, protocol::ControlMessageType::KEEPALIVE_PONG,
                     nullptr, 0);
        break;
    }

    // ── Channel join ────────────────────────────────────────────────────
    case protocol::ControlMessageType::CHANNEL_JOIN: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        ChannelId channel_id = reader.read_u32();
        if (reader.error()) break;

        // Verify channel exists
        auto channel = db_.get_channel(channel_id);
        if (!channel) {
            send_error(msg.session_id, "Channel not found");
            break;
        }

        // Check permission
        Role user_role = static_cast<Role>(session->role);
        auto ch_perm = db_.get_channel_permission(channel_id, user_role);
        if (!has_permission(user_role, Permission::JoinChannel, ch_perm)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        // Check max users (0 = use server default)
        int max = channel->max_users > 0 ? channel->max_users : config_.max_users_per_channel;
        if (max > 0) {
            uint32_t count = 0;
            auto all = quic_.get_sessions();
            for (auto& s : all) {
                if (s->authenticated && s->channel_id == channel_id)
                    count++;
            }
            if (static_cast<int>(count) >= max) {
                send_error(msg.session_id, "Channel is full");
                break;
            }
        }

        // Leave current channel if in one
        ChannelId old_channel = session->channel_id;
        if (old_channel != 0 && old_channel != channel_id) {
            stop_screen_share(old_channel, session->user_id);
            clear_viewer_subscriptions(session, true);
            session->channel_id = 0;
            BinaryWriter leave_writer;
            leave_writer.write_u32(session->user_id);
            leave_writer.write_u32(old_channel);
            auto all = quic_.get_sessions();
            for (auto& s : all) {
                if (s->id != msg.session_id && s->authenticated)
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                                   leave_writer.data().data(), leave_writer.data().size());
            }
        }

        session->channel_id = channel_id;
        LOG_INFO("User '{}' joined channel '{}' ({})", session->username, channel->name, channel_id);

        // Send user list for the channel
        {
            auto all = quic_.get_sessions();
            BinaryWriter list_writer;
            // Count users in channel
            uint32_t count = 0;
            for (auto& s : all) {
                if (s->authenticated && s->channel_id == channel_id)
                    count++;
            }
            list_writer.write_u32(channel_id);
            list_writer.write_u32(count);
            for (auto& s : all) {
                if (s->authenticated && s->channel_id == channel_id) {
                    list_writer.write_u32(s->user_id);
                    list_writer.write_string(s->username);
                    list_writer.write_u8(static_cast<uint8_t>(s->role));
                    list_writer.write_u8(s->muted ? 1 : 0);
                    list_writer.write_u8(s->deafened ? 1 : 0);
                }
            }
            quic_.send_to(msg.session_id, protocol::ControlMessageType::CHANNEL_USER_LIST,
                         list_writer.data().data(), list_writer.data().size());
        }

        // Notify new joiner about all active screen sharers in this channel.
        // The trailing replay flag (1) tells 1.2+ clients not to play the
        // "stream started" sound for shares that were already running.
        {
            std::vector<UserId> sharers;
            {
                std::lock_guard<std::mutex> lock(sharers_mutex_);
                auto ss_it = channel_screen_sharers_.find(channel_id);
                if (ss_it != channel_screen_sharers_.end())
                    sharers.assign(ss_it->second.begin(), ss_it->second.end());
            }
            for (UserId sharer_id : sharers) {
                auto s = session_for_user(sharer_id);
                if (!s || !s->authenticated) continue;
                BinaryWriter ss_writer;
                ss_writer.write_u32(s->user_id);
                ss_writer.write_u8(s->share_codec);
                ss_writer.write_u16(s->share_width);
                ss_writer.write_u16(s->share_height);
                ss_writer.write_u8(1);   // replay
                quic_.send_to(msg.session_id,
                             protocol::ControlMessageType::SCREEN_SHARE_STARTED,
                             ss_writer.data().data(), ss_writer.data().size());
            }
        }

        // Notify others in the channel
        {
            BinaryWriter join_writer;
            join_writer.write_u32(session->user_id);
            join_writer.write_string(session->username);
            join_writer.write_u32(channel_id);
            join_writer.write_u8(static_cast<uint8_t>(session->role));

            auto all = quic_.get_sessions();
            for (auto& s : all) {
                if (s->id != msg.session_id && s->authenticated) {
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_JOINED_CHANNEL,
                                   join_writer.data().data(), join_writer.data().size());
                }
            }
        }
        break;
    }

    // ── Voice state update (mute/deafen) ─────────────────────────────────
    case protocol::ControlMessageType::VOICE_STATE_UPDATE: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint8_t muted = reader.read_u8();
        uint8_t deafened = reader.read_u8();
        if (reader.error()) break;

        session->muted = (muted != 0);
        session->deafened = (deafened != 0);

        // Broadcast to others in channel: [user_id(4)][muted(1)][deafened(1)]
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u8(muted);
        writer.write_u8(deafened);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->id != msg.session_id && s->authenticated) {
                quic_.send_to(s->id, protocol::ControlMessageType::USER_VOICE_STATE,
                               writer.data().data(), writer.data().size());
            }
        }
        break;
    }

    // ── Channel leave ───────────────────────────────────────────────────
    case protocol::ControlMessageType::CHANNEL_LEAVE: {
        if (!session->authenticated || session->channel_id == 0) break;

        ChannelId old_channel = session->channel_id;
        stop_screen_share(old_channel, session->user_id);
        clear_viewer_subscriptions(session, true);
        session->channel_id = 0;

        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u32(old_channel);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->id != msg.session_id && s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                               writer.data().data(), writer.data().size());
        }
        break;
    }

    // ── Admin: create channel ───────────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_CREATE_CHANNEL: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::CreateChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        std::string name = reader.read_string();
        int max_users = static_cast<int>(reader.read_u32());
        if (reader.error() || name.empty()) break;

        if (!db_.create_channel(name, max_users)) {
            send_error(msg.session_id, "Failed to create channel");
            break;
        }

        LOG_INFO("Channel '{}' created by '{}'", name, session->username);

        // Broadcast updated channel list to all authenticated clients
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                send_channel_list(s->id);
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Channel created");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Admin: delete channel ───────────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_DELETE_CHANNEL: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::DeleteChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        ChannelId channel_id = reader.read_u32();
        if (reader.error()) break;

        // Kick everyone from the channel first. Tear down screen-share state
        // before the channel ids are zeroed: stop_screen_share needs the
        // channel to find the sharer and to notify the remaining members, and
        // a sharer that keeps video_ingress_allowed after leaving would still
        // have its per-frame streams buffered on the server.
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == channel_id) {
                stop_screen_share(channel_id, s->user_id);
                clear_viewer_subscriptions(s, false);
            }
        }
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == channel_id) {
                s->channel_id = 0;
                BinaryWriter leave_writer;
                leave_writer.write_u32(s->user_id);
                leave_writer.write_u32(channel_id);
                quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                               leave_writer.data().data(), leave_writer.data().size());
            }
        }

        if (!db_.delete_channel(channel_id)) {
            send_error(msg.session_id, "Failed to delete channel");
            break;
        }

        // Broadcast updated channel list
        for (auto& s : all) {
            if (s->authenticated)
                send_channel_list(s->id);
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Channel deleted");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Admin: rename channel ───────────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_RENAME_CHANNEL: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::CreateChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        ChannelId channel_id = reader.read_u32();
        std::string new_name = reader.read_string();
        if (reader.error() || new_name.empty()) break;

        if (!db_.rename_channel(channel_id, new_name)) {
            send_error(msg.session_id, "Failed to rename channel");
            break;
        }

        LOG_INFO("Channel {} renamed to '{}' by '{}'", channel_id, new_name, session->username);

        // Broadcast updated channel list to all authenticated clients
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                send_channel_list(s->id);
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Channel renamed");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Admin: set role ─────────────────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_SET_ROLE: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::ManageRoles)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        UserId target_id = reader.read_u32();
        uint8_t new_role = reader.read_u8();
        if (reader.error()) break;

        Role target_new_role = static_cast<Role>(new_role);

        // Owner role is assigned only via server config — never through API
        if (target_new_role == Role::Owner) {
            send_error(msg.session_id, "Owner role can only be set in server config");
            break;
        }

        // Check hierarchy: can't promote to equal or higher than self
        if (new_role <= session->role && user_role != Role::Owner) {
            send_error(msg.session_id, "Cannot assign a role equal or higher than your own");
            break;
        }

        // Check current target role
        auto target_user = db_.get_user_by_id(target_id);
        if (!target_user) {
            send_error(msg.session_id, "User not found");
            break;
        }

        if (!can_moderate(user_role, static_cast<Role>(target_user->role))) {
            send_error(msg.session_id, "Cannot modify a user with equal or higher role");
            break;
        }

        db_.set_user_role(target_id, target_new_role);

        // Update live session if online
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->user_id == target_id)
                s->role = new_role;
        }

        // Broadcast role change to all authenticated clients
        {
            BinaryWriter role_writer;
            role_writer.write_u32(target_id);
            role_writer.write_u8(new_role);
            for (auto& s : all) {
                if (s->authenticated) {
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_ROLE_CHANGED,
                                   role_writer.data().data(), role_writer.data().size());
                }
            }
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Role updated");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Admin: kick user ────────────────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_KICK_USER: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::KickFromServer)) {
            send_error(msg.session_id, "Permission denied",
                       protocol::ServerErrorCode::PermissionDenied);
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        UserId target_id = reader.read_u32();
        if (reader.error()) break;

        // Find target session
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->user_id == target_id && s->authenticated) {
                if (!can_moderate(user_role, static_cast<Role>(s->role))) {
                    send_error(msg.session_id, "Cannot kick a user with equal or higher role",
                               protocol::ServerErrorCode::PermissionDenied);
                    break;
                }
                send_error(s->id, "You have been kicked from the server",
                           protocol::ServerErrorCode::Kicked);
                quic_.disconnect(s->id);
            }
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("User kicked");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Screen share start ───────────────────────────────────────────────
    case protocol::ControlMessageType::SCREEN_SHARE_START: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint8_t codec_id = reader.read_u8();
        uint16_t width = reader.read_u16();
        uint16_t height = reader.read_u16();
        if (reader.error()) break;

        ChannelId ch = session->channel_id;

        // Allow multiple sharers per channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            channel_screen_sharers_[ch].insert(session->user_id);
        }
        session->share_codec = codec_id;
        session->share_width = width;
        session->share_height = height;
        // From now on this session's per-frame video streams are accepted
        // (release pairs with the acquire in QuicServer's PEER_STREAM_STARTED).
        session->video_ingress_allowed.store(true, std::memory_order_release);

        // Notify all in channel (including sender for confirmation). Trailing
        // replay = 0: a live start (clients play the "stream started" sound).
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u8(codec_id);
        writer.write_u16(width);
        writer.write_u16(height);
        writer.write_u8(0);   // replay

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == ch) {
                quic_.send_to(s->id, protocol::ControlMessageType::SCREEN_SHARE_STARTED,
                               writer.data().data(), writer.data().size());
            }
        }

        LOG_INFO("User '{}' started screen sharing in channel {} ({}x{})", session->username, ch, width, height);
        break;
    }

    // ── Screen share update (encoder initialized with real codec/dims) ──
    case protocol::ControlMessageType::SCREEN_SHARE_UPDATE: {
        if (!session->authenticated || session->channel_id == 0) break;

        // Only accept from active sharers
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_screen_sharers_.find(session->channel_id);
            if (it == channel_screen_sharers_.end() ||
                it->second.count(session->user_id) == 0)
                break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint8_t codec_id = reader.read_u8();
        uint16_t width = reader.read_u16();
        uint16_t height = reader.read_u16();
        if (reader.error()) break;

        session->share_codec = codec_id;
        session->share_width = width;
        session->share_height = height;

        LOG_INFO("User '{}' encoder ready: codec={} {}x{}", session->username, codec_id, width, height);
        break;
    }

    // ── Screen share stop ────────────────────────────────────────────────
    case protocol::ControlMessageType::SCREEN_SHARE_STOP: {
        if (!session->authenticated || session->channel_id == 0) break;
        stop_screen_share(session->channel_id, session->user_id);
        break;
    }

    // ── Screen share view (subscribe/unsubscribe) ────────────────────────
    // Wire formats (a viewer may watch several sharers at once):
    //   [target(4)]            legacy single-select: replace the whole set with
    //                          {target} (target 0 = clear all).
    //   [target(4)][action(1)] additive: action 1 = add target, 0 = remove
    //                          target (target 0 + action 0 = clear all).
    case protocol::ControlMessageType::SCREEN_SHARE_VIEW: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t target_id = reader.read_u32();
        if (reader.error()) break;

        const bool additive = msg.payload.size() >= 5;
        const bool subscribe = additive ? (reader.read_u8() != 0) : (target_id != 0);

        if (!subscribe) {
            if (target_id == 0) {
                clear_viewer_subscriptions(session, true);   // clear all
                break;
            }
            bool was_subscribed = false;
            {
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                was_subscribed = session->subscribed_sharers.erase(target_id) != 0;
                session->video_gates.erase(target_id);
            }
            if (was_subscribed)
                notify_viewer_change(target_id, session->user_id, false);
            break;
        }
        if (target_id == 0) break;

        // Verify target is actually sharing in this channel
        bool is_sharer = false;
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_screen_sharers_.find(session->channel_id);
            is_sharer = (it != channel_screen_sharers_.end() &&
                         it->second.count(target_id));
        }
        if (!is_sharer) break;

        std::vector<UserId> dropped;   // sharers replaced by a legacy single-select
        bool newly_subscribed = false;
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            // Legacy single-select replaces the whole set; additive keeps the
            // rest. Only the OTHER sharers are dropped: a repeated select of
            // the same target keeps its gate and, being no new subscription,
            // triggers neither the auto-PLI nor a viewer notification — so a
            // client re-sending its selection cannot spam the sharer.
            if (!additive) {
                for (auto it = session->subscribed_sharers.begin();
                     it != session->subscribed_sharers.end(); ) {
                    if (*it == target_id) { ++it; continue; }
                    dropped.push_back(*it);
                    session->video_gates.erase(*it);
                    it = session->subscribed_sharers.erase(it);
                }
            }
            newly_subscribed = session->subscribed_sharers.insert(target_id).second;
            if (newly_subscribed) {
                // A fresh viewer cannot use anything before the next keyframe:
                // gate delivery until it arrives (forward_video_frame keeps
                // asking the sharer, coalesced, until it does).
                session->video_gates[target_id].awaiting_keyframe = true;
            }
        }
        for (UserId old : dropped)
            notify_viewer_change(old, session->user_id, false);

        if (newly_subscribed) {
            // Auto-PLI: tell the sharer to send a keyframe so the new viewer
            // can decode from the Sequence Header (coalesced per sharer).
            request_keyframe_from_sharer(target_id, session->user_id);
            notify_viewer_change(target_id, session->user_id, true);
        }
        break;
    }

    // ── Admin: create text channel ────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_CREATE_TEXT_CHANNEL: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::CreateChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        std::string name = reader.read_string();
        if (reader.error() || name.empty()) break;

        if (!db_.create_text_channel(name)) {
            send_error(msg.session_id, "Failed to create text channel");
            break;
        }

        LOG_INFO("Text channel '{}' created by '{}'", name, session->username);

        // Broadcast updated text channel list to all authenticated clients
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                send_text_channel_list(s->id);
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Text channel created");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Admin: delete text channel ────────────────────────────────────
    case protocol::ControlMessageType::ADMIN_DELETE_TEXT_CHANNEL: {
        if (!session->authenticated) break;

        Role user_role = static_cast<Role>(session->role);
        if (!has_permission(user_role, Permission::DeleteChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        if (reader.error()) break;

        if (!db_.delete_text_channel(channel_id)) {
            send_error(msg.session_id, "Failed to delete text channel");
            break;
        }

        LOG_INFO("Text channel {} deleted by '{}'", channel_id, session->username);

        // Broadcast updated text channel list
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                send_text_channel_list(s->id);
        }

        BinaryWriter writer;
        writer.write_u8(1);
        writer.write_string("Text channel deleted");
        quic_.send_to(msg.session_id, protocol::ControlMessageType::ADMIN_RESULT,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Chat: send message ───────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_SEND: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        std::string text = reader.read_string();
        uint8_t attachment_count = reader.read_u8();
        if (reader.error()) break;

        // Validate channel exists
        auto ch = db_.get_text_channel(channel_id);
        if (!ch) {
            send_error(msg.session_id, "Text channel not found");
            break;
        }

        // Validate message length
        if (static_cast<int>(text.size()) > config_.chat.max_message_length) {
            send_error(msg.session_id, "Message too long");
            break;
        }

        auto now = static_cast<uint64_t>(std::time(nullptr));
        uint64_t msg_id = db_.insert_message(channel_id, session->user_id,
                                              session->username, text, now);
        if (msg_id == 0) {
            send_error(msg.session_id, "Failed to store message");
            break;
        }

        // Read attachment metadata and create DB entries
        struct PendingAttachment { std::string name; uint64_t size; std::string mime; uint64_t db_id; };
        std::vector<PendingAttachment> attachments;
        for (uint8_t i = 0; i < attachment_count; i++) {
            std::string fname = reader.read_string();
            uint64_t fsize = reader.read_u64();
            std::string mime = reader.read_string();
            if (reader.error()) break;

            // Generate disk path. NEVER derive it from the client filename:
            // `fname` could contain path-traversal (`..\`, absolute paths) and
            // escape the storage root. The on-disk name is fully server-
            // controlled (msg_id + index + a sanitized extension); the original
            // display name is preserved separately in the DB (`fname` below).
            auto t = std::time(nullptr);
            std::tm tm_buf{};
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &t);
#else
            gmtime_r(&t, &tm_buf);
#endif
            char dir[16];
            std::strftime(dir, sizeof(dir), "%Y-%m", &tm_buf);
            std::string ext;
            if (auto dot = fname.find_last_of('.');
                dot != std::string::npos && dot + 1 < fname.size()) {
                for (char c : fname.substr(dot)) {  // keep the leading dot
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.')
                        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ext.size() >= 8) break;
                }
            }
            std::string disk_path = std::string(dir) + "/" +
                std::to_string(msg_id) + "_" + std::to_string(i) + ext;

            uint64_t att_id = db_.insert_attachment(msg_id, fname,
                static_cast<int64_t>(fsize), mime, disk_path);
            attachments.push_back({fname, fsize, mime, att_id});
        }

        // Build CHAT_MESSAGE and broadcast to all authenticated clients
        BinaryWriter writer;
        writer.write_u64(msg_id);
        writer.write_u32(channel_id);
        writer.write_u32(session->user_id);
        writer.write_string(session->username);
        writer.write_u64(now);
        writer.write_string(text);
        writer.write_u8(0); // pinned = false
        writer.write_u8(static_cast<uint8_t>(attachments.size()));
        for (auto& att : attachments) {
            writer.write_u64(att.db_id);
            writer.write_string(att.name);
            writer.write_u64(att.size);
            writer.write_string(att.mime);
            writer.write_u8(0); // uploaded = false
        }

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::CHAT_MESSAGE,
                             writer.data().data(), writer.data().size());
        }
        break;
    }

    // ── Chat: history request ─────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_HISTORY_REQ: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        uint64_t before_id = reader.read_u64();
        uint16_t limit = reader.read_u16();
        if (reader.error()) break;

        // Cap limit server-side
        if (limit > 100) limit = 100;

        auto messages = db_.get_messages(channel_id, before_id, limit);

        // Check if there are more messages before this batch
        uint8_t has_more = 0;
        if (!messages.empty()) {
            auto oldest = db_.get_messages(channel_id, messages.front().id, 1);
            has_more = oldest.empty() ? 0 : 1;
        }

        BinaryWriter writer;
        writer.write_u32(channel_id);
        writer.write_u8(has_more);
        writer.write_u16(static_cast<uint16_t>(messages.size()));

        for (auto& m : messages) {
            writer.write_u64(m.id);
            writer.write_u32(m.channel_id);
            writer.write_u32(m.sender_id);
            writer.write_string(m.sender_name);
            writer.write_u64(m.created_at);
            writer.write_string(m.text);
            writer.write_u8(m.pinned ? 1 : 0);

            auto atts = db_.get_attachments_for_message(m.id);
            writer.write_u8(static_cast<uint8_t>(atts.size()));
            for (auto& a : atts) {
                writer.write_u64(a.id);
                writer.write_string(a.file_name);
                writer.write_u64(static_cast<uint64_t>(a.file_size));
                writer.write_string(a.mime_type);
                writer.write_u8(a.uploaded ? 1 : 0);
            }
        }

        quic_.send_to(msg.session_id, protocol::ControlMessageType::CHAT_HISTORY_RESP,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Chat: pin message ─────────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_PIN: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint64_t message_id = reader.read_u64();
        if (reader.error()) break;

        auto message = db_.get_message(message_id);
        if (!message) break;

        db_.pin_message(message_id);

        // Broadcast updated message to all
        message->pinned = true;
        BinaryWriter writer;
        writer.write_u64(message->id);
        writer.write_u32(message->channel_id);
        writer.write_u32(message->sender_id);
        writer.write_string(message->sender_name);
        writer.write_u64(message->created_at);
        writer.write_string(message->text);
        writer.write_u8(1); // pinned
        writer.write_u8(0); // no attachments in update

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::CHAT_MESSAGE,
                             writer.data().data(), writer.data().size());
        }
        break;
    }

    // ── Chat: unpin message ───────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_UNPIN: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint64_t message_id = reader.read_u64();
        if (reader.error()) break;

        auto message = db_.get_message(message_id);
        if (!message) break;

        db_.unpin_message(message_id);

        message->pinned = false;
        BinaryWriter writer;
        writer.write_u64(message->id);
        writer.write_u32(message->channel_id);
        writer.write_u32(message->sender_id);
        writer.write_string(message->sender_name);
        writer.write_u64(message->created_at);
        writer.write_string(message->text);
        writer.write_u8(0); // unpinned
        writer.write_u8(0);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::CHAT_MESSAGE,
                             writer.data().data(), writer.data().size());
        }
        break;
    }

    // ── Chat: delete message ──────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_DELETE: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint64_t message_id = reader.read_u64();
        if (reader.error()) break;

        auto message = db_.get_message(message_id);
        if (!message) break;

        // Only author or moderator+ can delete
        Role user_role = static_cast<Role>(session->role);
        if (message->sender_id != session->user_id &&
            !has_permission(user_role, Permission::KickFromChannel)) {
            send_error(msg.session_id, "Permission denied");
            break;
        }

        db_.soft_delete_message(message_id);

        BinaryWriter writer;
        writer.write_u64(message_id);
        writer.write_u32(message->channel_id);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated)
                quic_.send_to(s->id, protocol::ControlMessageType::CHAT_MESSAGE_DELETED,
                             writer.data().data(), writer.data().size());
        }
        break;
    }

    // ── Chat: pinned messages request ─────────────────────────────────
    case protocol::ControlMessageType::CHAT_PINNED_REQ: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        if (reader.error()) break;

        auto messages = db_.get_pinned_messages(channel_id);

        BinaryWriter writer;
        writer.write_u32(channel_id);
        writer.write_u16(static_cast<uint16_t>(messages.size()));

        for (auto& m : messages) {
            writer.write_u64(m.id);
            writer.write_u32(m.channel_id);
            writer.write_u32(m.sender_id);
            writer.write_string(m.sender_name);
            writer.write_u64(m.created_at);
            writer.write_string(m.text);
            writer.write_u8(m.pinned ? 1 : 0);
            writer.write_u8(0); // no attachments in pinned list
        }

        quic_.send_to(msg.session_id, protocol::ControlMessageType::CHAT_PINNED_RESP,
                     writer.data().data(), writer.data().size());
        break;
    }

    // ── Chat: search ──────────────────────────────────────────────────
    case protocol::ControlMessageType::CHAT_SEARCH: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        std::string query = reader.read_string();
        uint64_t before_id = reader.read_u64();
        uint16_t limit = reader.read_u16();
        if (reader.error() || query.empty()) break;

        if (limit > 50) limit = 50;

        auto results = db_.search_messages(channel_id, query, before_id, limit);

        BinaryWriter writer;
        writer.write_u32(channel_id);
        writer.write_u16(static_cast<uint16_t>(results.size()));

        for (auto& m : results) {
            writer.write_u64(m.id);
            writer.write_u32(m.channel_id);
            writer.write_u32(m.sender_id);
            writer.write_string(m.sender_name);
            writer.write_u64(m.created_at);
            writer.write_string(m.text);
            writer.write_u8(m.pinned ? 1 : 0);
            writer.write_u8(0);
        }

        quic_.send_to(msg.session_id, protocol::ControlMessageType::CHAT_SEARCH_RESP,
                     writer.data().data(), writer.data().size());
        break;
    }

    default:
        LOG_WARN("Unhandled message type {:#06x} from session {}", static_cast<unsigned>(msg.type), msg.session_id);
        break;
    }
}

void Server::process_disconnects() {
	ZoneScopedN("Server::process_disconnects");
    for (auto& d : quic_.disconnects().drain())
        process_disconnect(d.session_id, d.user_id, d.channel_id);
}

void Server::process_disconnect(uint32_t session_id, UserId user_id, ChannelId channel_id) {
	ZoneScopedN("Server::process_disconnect");
    // Per-session bookkeeping first (independent of channel membership).
    {
        std::lock_guard<std::mutex> lock(user_index_mutex_);
        auto it = user_sessions_.find(user_id);
        if (it != user_sessions_.end() && it->second == session_id)
            user_sessions_.erase(it);
    }
    video_control_buckets_.erase(session_id);

    std::shared_ptr<Session> session;
    if (auto it = authed_sessions_.find(session_id); it != authed_sessions_.end()) {
        session = it->second;
        authed_sessions_.erase(it);
        // The user index no longer points at this session, so the
        // stop_screen_share below cannot clear this for us.
        session->video_ingress_allowed.store(false, std::memory_order_release);
    }

    if (channel_id == 0)
        return;

    // Clean up screen share if this user was sharing.
    stop_screen_share(channel_id, user_id);

    // Drop what this viewer was watching; tell those sharers (best effort —
    // the sharer may itself be gone).
    if (session)
        clear_viewer_subscriptions(session, true);

    BinaryWriter writer;
    writer.write_u32(user_id);
    writer.write_u32(channel_id);

    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->id != session_id && s->authenticated) {
            quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                           writer.data().data(), writer.data().size());
        }
    }
}

void Server::enqueue_video_frame(uint32_t session_id, const uint8_t* data, size_t len) {
    if (len < VIDEO_FRAME_HEADER_SIZE || len > VIDEO_FRAME_MAX_PAYLOAD_BYTES) return;
    auto session = quic_.get_session(session_id);
    if (!session) return;
    const auto generation = session->video_generation.load(std::memory_order_acquire);
    if (!session->video_ingress_allowed.load(std::memory_order_acquire)) return;
    const auto bytes = static_cast<int64_t>(len);
    const auto pending = session->video_pending_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    if (pending > Session::VIDEO_INGRESS_MAX_BUFFERED_BYTES) {
        session->video_pending_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        return;
    }
    video_incoming_.push({session, generation, std::vector<uint8_t>(data, data + len)});
}

void Server::process_video_frames() {
    auto pending = video_incoming_.drain();
    for (auto& frame : pending) {
        const auto& session = frame.session;
        session->video_pending_bytes.fetch_sub(static_cast<int64_t>(frame.data.size()),
                                               std::memory_order_relaxed);
        if (!session->alive || !session->video_ingress_allowed.load(std::memory_order_acquire) ||
            frame.generation != session->video_generation.load(std::memory_order_acquire)) continue;
        VideoFrameHeader hdr;
        if (!VideoFrameHeader::parse(frame.data.data(), frame.data.size(), hdr)) continue;
        auto& ingress = video_ingress_[session->id];
        ingress.session = session;
        ingress.reorder.on_frame(hdr, std::move(frame.data), now_us(),
            [this, id = session->id](const VideoFrameHeader&, std::vector<uint8_t>&& data) {
                forward_video_frame(id, data.data(), data.size());
            },
            [this, user = session->user_id](uint32_t) { request_keyframe_from_sharer(user, 0); });
    }
    const int64_t now = now_us();
    for (auto it = video_ingress_.begin(); it != video_ingress_.end();) {
        const auto& session = it->second.session;
        if (!session->alive || !session->video_ingress_allowed.load(std::memory_order_acquire)) {
            it = video_ingress_.erase(it);
            continue;
        }
        it->second.reorder.poll(now,
            [this, id = session->id](const VideoFrameHeader&, std::vector<uint8_t>&& data) {
                forward_video_frame(id, data.data(), data.size());
            },
            [this, user = session->user_id](uint32_t) { request_keyframe_from_sharer(user, 0); });
        // Recovery no longer forwards undecodable deltas. Keep retrying even
        // when the sharer is idle or the requested keyframe is itself lost.
        if (it->second.reorder.needs_keyframe())
            request_keyframe_from_sharer(session->user_id, 0);
        ++it;
    }
}

void Server::forward_video_frame(uint32_t session_id, const uint8_t* data, size_t len) {
	ZoneScopedN("Server::forward_video_frame");
    auto session = quic_.get_session(session_id);
    if (!session || !session->authenticated || session->channel_id == 0)
        return;

    // Verify this user is an active screen sharer in the channel
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_screen_sharers_.find(session->channel_id);
        if (it == channel_screen_sharers_.end() ||
            it->second.count(session->user_id) == 0)
            return;
    }

    VideoFrameHeader hdr;
    if (!VideoFrameHeader::parse(data, len, hdr)) {
        LOG_DEBUG("Session {}: video frame too short ({} bytes)", session_id, len);
        return;
    }
    const bool      keyframe = hdr.keyframe();
    const UserId    sharer   = session->user_id;
    const ChannelId channel  = session->channel_id;
    const int64_t   now      = now_us();

    // Per-viewer backlog budget inputs. A viewer of several sharers has ONE
    // outstanding-bytes counter, so its threshold is the SUM of the rates of
    // every sharer it watches x (its MinRtt + 250 ms) plus headroom for the
    // largest keyframe this sharer produced recently (one admitted keyframe
    // must not by itself trip the gate for the deltas behind it). Until a
    // full window has been observed, assume this frame size at 30 fps for
    // this sharer and 0 for sharers without an estimate yet.
    std::unordered_map<UserId, double> sharer_rates;
    int64_t keyframe_headroom = 0;
    double  sharer_rate = 0.0;
    {
        std::lock_guard<std::mutex> lock(video_state_mutex_);
        auto& st = sharer_video_state_[sharer];
        st.rate.add(now, len);
        st.last_frame_seq = hdr.frame_seq;
        st.have_frame = true;
        if (keyframe) {
            const int64_t size  = static_cast<int64_t>(len);
            const bool    stale = st.max_keyframe_at_us == 0 ||
                                  now - st.max_keyframe_at_us > KEYFRAME_HEADROOM_TTL_US;
            if (stale || size > st.max_keyframe_bytes) {
                st.max_keyframe_bytes = size;
                st.max_keyframe_at_us = now;
            }
        }
        keyframe_headroom = st.max_keyframe_bytes;
        if (keyframe)
            keyframe_headroom = std::max<int64_t>(keyframe_headroom, static_cast<int64_t>(len));

        sharer_rates.reserve(sharer_video_state_.size());
        for (auto& [uid, s] : sharer_video_state_) {
            const double fallback = (uid == sharer) ? static_cast<double>(len) * 30.0 : 0.0;
            sharer_rates[uid] = s.rate.bytes_per_second(fallback);
        }
        sharer_rate = sharer_rates[sharer];
    }

    // Build the forwarded frame ONCE: [sender_id(4)][header(14)][encoded].
    // send_video_frame_to prepends the per-path type/length bytes and shares
    // this buffer across every viewer's send context (no copy per viewer).
    auto frame = std::make_shared<std::vector<uint8_t>>();
    frame->resize(4 + len);
    uint32_t uid = sharer;
    std::memcpy(frame->data(), &uid, 4);
    std::memcpy(frame->data() + 4, data, len);
    std::shared_ptr<const std::vector<uint8_t>> shared_frame = std::move(frame);

    // Snapshot the admitted viewers under subscriptions_mutex_ (this runs on
    // the receive thread, concurrent with main-loop subscription mutations),
    // applying the per-viewer backlog gate, then send outside the lock so a
    // stalled send can't block subscribe/unsubscribe.
    std::vector<std::shared_ptr<Session>> targets;
    bool     want_keyframe = false;
    uint32_t dropped = 0;
    {
        auto all_sessions = quic_.get_sessions();
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (auto& s : all_sessions) {
            if (s->id == session_id ||
                !s->authenticated ||
                s->channel_id != channel ||
                !s->subscribed_sharers.count(sharer))
                continue;

            // Budget of THIS viewer: all its sharers' rates, its own MinRtt.
            const int64_t threshold = viewer_backlog_threshold(*s, sharer_rates, keyframe_headroom);
            auto& gate = s->video_gates[sharer];
            bool needs_key = false;
            if (gate.admit(s->video_outstanding_bytes.load(std::memory_order_relaxed),
                           threshold, keyframe, &needs_key)) {
                targets.push_back(s);
            } else {
                ++dropped;
            }
            if (needs_key) want_keyframe = true;
        }
    }

    for (auto& s : targets) {
        if (!quic_.send_video_frame_to(s, shared_frame)) {
            // Nothing was queued for this viewer: it missed a frame, so it can
            // only resume at the next keyframe.
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            s->video_gates[sharer].awaiting_keyframe = true;
            want_keyframe = true;
            ++dropped;
        }
    }

    if (dropped) {
        std::lock_guard<std::mutex> lock(video_state_mutex_);
        auto& st = sharer_video_state_[sharer];
        st.drops_since_log += dropped;
        if (st.drop_log.allow(now)) {
            LOG_DEBUG("Sharer {}: dropped {} viewer-frame(s) over backlog threshold in the last interval (sharer rate {:.0f} B/s, keyframe headroom {} B)",
                      sharer, st.drops_since_log, sharer_rate, keyframe_headroom);
            st.drops_since_log = 0;
        }
    }

    if (want_keyframe)
        request_keyframe_from_sharer(sharer, 0);
}

int64_t Server::viewer_backlog_threshold(const Session& viewer,
                                         const std::unordered_map<UserId, double>& sharer_rates,
                                         int64_t keyframe_headroom) const {
    // Every sharer this viewer watches feeds the same outstanding-bytes
    // counter (a sharer without an estimate contributes 0).
    double viewer_rate = 0.0;
    for (UserId sharer : viewer.subscribed_sharers) {
        auto it = sharer_rates.find(sharer);
        if (it != sharer_rates.end()) viewer_rate += it->second;
    }
    return video_backlog_threshold_bytes(viewer_rate,
                                         viewer.min_rtt_us.load(std::memory_order_relaxed),
                                         keyframe_headroom);
}

void Server::handle_video_control(const DataPacket& pkt) {
	ZoneScopedN("Server::handle_video_control");
    // Only an authenticated, in-channel session gets a token bucket: anything
    // else is dropped before any per-session state exists for it, so an
    // unauthenticated peer cannot grow video_control_buckets_ (entries are
    // only erased by process_disconnect, which never sees such sessions).
    auto session = quic_.get_session(pkt.session_id);
    if (!session || !session->authenticated || session->channel_id == 0)
        return;

    // Rate limit before any further work: 4 control messages/s per session, burst 4.
    {
        auto it = video_control_buckets_.find(pkt.session_id);
        if (it == video_control_buckets_.end())
            it = video_control_buckets_.emplace(pkt.session_id, TokenBucket(4.0, 4.0)).first;
        if (!it->second.try_take(now_us()))
            return;
    }

    if (pkt.data.size() < 2) return;
    uint8_t subtype = pkt.data[0];

    if (subtype == protocol::VIDEO_CTL_PLI) {
        // Client sends: [subtype(1)][target_user_id(4)]
        if (pkt.data.size() < 5) return;
        uint32_t target_id;
        std::memcpy(&target_id, pkt.data.data() + 1, 4);

        // Verify target is an active sharer in the requester's channel (the
        // sharer table is keyed by channel, so this also proves same-channel).
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_screen_sharers_.find(session->channel_id);
            if (it == channel_screen_sharers_.end() ||
                it->second.count(target_id) == 0)
                return;
        }

        // A viewer we are currently throttling (over its backlog threshold and
        // already waiting for a keyframe) gets nothing from a new keyframe
        // right now — it would be dropped too. forward_video_frame asks for
        // one as soon as the viewer is back under budget. The threshold is
        // computed exactly as forward_video_frame does it: every sharer's
        // rate (no estimate yet = 0) and the target's keyframe headroom.
        std::unordered_map<UserId, double> sharer_rates;
        int64_t keyframe_headroom = 0;
        {
            std::lock_guard<std::mutex> lock(video_state_mutex_);
            sharer_rates.reserve(sharer_video_state_.size());
            for (auto& [uid, st] : sharer_video_state_)
                sharer_rates[uid] = st.rate.bytes_per_second(0.0);
            auto st = sharer_video_state_.find(target_id);
            if (st != sharer_video_state_.end())
                keyframe_headroom = st->second.max_keyframe_bytes;
        }
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto g = session->video_gates.find(target_id);
            if (g != session->video_gates.end() && g->second.awaiting_keyframe) {
                const int64_t threshold =
                    viewer_backlog_threshold(*session, sharer_rates, keyframe_headroom);
                if (session->video_outstanding_bytes.load(std::memory_order_relaxed) > threshold)
                    return;
            }
        }

        request_keyframe_from_sharer(target_id, session->user_id);
    }
}

void Server::request_keyframe_from_sharer(UserId sharer_user_id, UserId requester) {
	ZoneScopedN("Server::request_keyframe_from_sharer");
    // Coalesce: one PLI per sharer per 200 ms, whoever asks.
    {
        std::lock_guard<std::mutex> lock(video_state_mutex_);
        if (!sharer_video_state_[sharer_user_id].pli.allow(now_us()))
            return;
    }

    auto sharer = session_for_user(sharer_user_id);
    if (!sharer || !sharer->authenticated) return;

    // [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][requester u32] as a priority datagram
    // so it isn't queued behind video.
    uint8_t pli[6];
    pli[0] = protocol::VIDEO_CONTROL_TYPE;
    pli[1] = protocol::VIDEO_CTL_PLI;
    uint32_t requester_id = requester;
    std::memcpy(pli + 2, &requester_id, 4);
    quic_.send_datagram(sharer->id, pli, sizeof(pli), true);
}

void Server::notify_viewer_change(UserId sharer_user_id, UserId viewer_user_id, bool watching) {
    if (sharer_user_id == viewer_user_id) return;   // self-preview subscription

    auto sharer = session_for_user(sharer_user_id);
    if (!sharer || !sharer->authenticated) return;
    // Pre-1.2 clients would log SCREEN_SHARE_VIEWER as an unhandled message.
    if (!sharer->supports_frame_streams()) return;

    BinaryWriter writer;
    writer.write_u32(viewer_user_id);
    writer.write_u8(watching ? 1 : 0);
    quic_.send_to(sharer->id, protocol::ControlMessageType::SCREEN_SHARE_VIEWER,
                  writer.data().data(), writer.data().size());
}

std::shared_ptr<Session> Server::session_for_user(UserId user_id) {
    uint32_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(user_index_mutex_);
        auto it = user_sessions_.find(user_id);
        if (it == user_sessions_.end()) return nullptr;
        session_id = it->second;
    }
    // Takes sessions_mutex_: never call this while holding it or a quic_mutex.
    return quic_.get_session(session_id);
}

void Server::clear_viewer_subscriptions(const std::shared_ptr<Session>& session, bool notify) {
    std::vector<UserId> sharers;
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        sharers.assign(session->subscribed_sharers.begin(), session->subscribed_sharers.end());
        session->subscribed_sharers.clear();
        session->video_gates.clear();
    }
    if (!notify) return;
    for (UserId sharer : sharers)
        notify_viewer_change(sharer, session->user_id, false);
}

void Server::forward_stream_audio(const DataPacket& pkt) {
	ZoneScopedN("Server::forward_stream_audio");
    auto session = quic_.get_session(pkt.session_id);
    if (!session || !session->authenticated || session->channel_id == 0)
        return;

    // Verify this user is an active screen sharer
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_screen_sharers_.find(session->channel_id);
        if (it == channel_screen_sharers_.end() ||
            it->second.count(session->user_id) == 0)
            return;
    }

    // Forward: [STREAM_AUDIO_PACKET_TYPE][sender_id(4)][opus_data]
    std::vector<uint8_t> fwd;
    fwd.reserve(1 + 4 + pkt.data.size());
    fwd.push_back(protocol::STREAM_AUDIO_PACKET_TYPE);
    uint32_t uid = session->user_id;
    fwd.insert(fwd.end(), reinterpret_cast<uint8_t*>(&uid),
               reinterpret_cast<uint8_t*>(&uid) + 4);
    fwd.insert(fwd.end(), pkt.data.begin(), pkt.data.end());

    // Forward to viewers subscribed to this sharer via datagram. Snapshot under
    // subscriptions_mutex_ for parity with forward_video_frame (see that comment).
    std::vector<uint32_t> targets;
    {
        auto all_sessions = quic_.get_sessions();
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (auto& s : all_sessions) {
            if (s->id != pkt.session_id &&
                s->authenticated &&
                s->channel_id == session->channel_id &&
                s->subscribed_sharers.count(session->user_id)) {
                targets.push_back(s->id);
            }
        }
    }
    for (uint32_t sid : targets)
        quic_.send_datagram(sid, fwd.data(), fwd.size());
}

void Server::stop_screen_share(ChannelId channel_id, UserId user_id) {
    for (auto it = video_ingress_.begin(); it != video_ingress_.end();) {
        if (it->second.session->user_id == user_id) it = video_ingress_.erase(it);
        else ++it;
    }
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_screen_sharers_.find(channel_id);
        if (it == channel_screen_sharers_.end()) return;
        if (it->second.erase(user_id) == 0) return; // wasn't sharing
        if (it->second.empty())
            channel_screen_sharers_.erase(it);
    }

    // No longer a sharer: new per-frame video streams from this session are
    // refused at PEER_STREAM_STARTED. (On the disconnect path the user index
    // no longer resolves — process_disconnect clears the flag itself there.)
    if (auto sharer = session_for_user(user_id)) {
        sharer->video_ingress_allowed.store(false, std::memory_order_release);
        sharer->video_generation.fetch_add(1, std::memory_order_release);
    }

    // Clear subscriptions (and backlog gates) pointing to this sharer
    auto all = quic_.get_sessions();
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (auto& s : all) {
            s->subscribed_sharers.erase(user_id);
            s->video_gates.erase(user_id);
        }
    }
    // Forwarding state (rate estimate, PLI coalescer) restarts on the next share
    {
        std::lock_guard<std::mutex> lock(video_state_mutex_);
        sharer_video_state_.erase(user_id);
    }

    // Notify all in channel
    BinaryWriter writer;
    writer.write_u32(user_id);

    for (auto& s : all) {
        if (s->authenticated && s->channel_id == channel_id) {
            quic_.send_to(s->id, protocol::ControlMessageType::SCREEN_SHARE_STOPPED,
                           writer.data().data(), writer.data().size());
        }
    }

    LOG_INFO("User {} stopped screen sharing in channel {}", user_id, channel_id);
}

} // namespace parties::server
