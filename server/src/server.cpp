#include <server/server.h>
#include <parties/crypto.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/types.h>
#include <parties/permissions.h>
#include <parties/video_common.h>
#include <parties/profiler.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

namespace parties::server {

Server::Server() = default;
Server::~Server() { stop(); }

bool Server::start(const Config& cfg) {
	ZoneScopedN("Server::start");
    config_ = cfg;

    // Open database
    if (!db_.open(config_.db_path)) {
        std::fprintf(stderr, "[Server] Failed to open database\n");
        return false;
    }

    if (!config_.root_fingerprints.empty()) {
        std::printf("[Server] %zu root fingerprint(s) configured\n",
                    config_.root_fingerprints.size());
    }

    // Generate self-signed cert if not present
    if (!std::filesystem::exists(config_.cert_file) ||
        !std::filesystem::exists(config_.key_file)) {
        std::printf("[Server] Generating self-signed certificate...\n");
        if (!parties::generate_self_signed_cert(config_.server_name,
                                                 config_.cert_file,
                                                 config_.key_file)) {
            std::fprintf(stderr, "[Server] Failed to generate certificate\n");
            return false;
        }
        std::printf("[Server] Certificate written to %s / %s\n",
                    config_.cert_file.c_str(), config_.key_file.c_str());
    }

    // Start QUIC transport (unified control + data plane)
    if (!quic_.start(config_.listen_ip, config_.port,
                     static_cast<size_t>(config_.max_clients),
                     config_.cert_file, config_.key_file)) {
        return false;
    }

    quic_.on_disconnect = [this](uint32_t session_id) {
        on_client_disconnect(session_id);
    };

    // Forward video frames directly from QUIC receive thread,
    // bypassing the polling loop to eliminate up to 1ms latency per frame.
    quic_.on_video_frame = [this](uint32_t session_id, uint8_t packet_type,
                                  const uint8_t* data, size_t len) {
        if (packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            forward_video_frame(session_id, data, len);
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

    running_ = true;
    std::printf("[Server] %s started successfully\n", config_.server_name.c_str());
    return true;
}

void Server::run() {
    while (running_) {
        ZoneScopedN("Server::run");
        process_control_messages();
        process_data_packets();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Server::stop() {
    if (!running_) return;
    running_ = false;
    quic_.stop();
    db_.close();
    std::printf("[Server] Stopped\n");
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
        if (pkt.packet_type == protocol::VOICE_PACKET_TYPE) {
            auto session = quic_.get_session(pkt.session_id);
            if (!session || !session->authenticated || session->channel_id == 0)
                continue;

            // Forward: [VOICE_PACKET_TYPE][sender_user_id(u32)][opus_data]
            std::vector<uint8_t> fwd;
            fwd.reserve(1 + 4 + pkt.data.size());
            fwd.push_back(protocol::VOICE_PACKET_TYPE);
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
            if (!targets.empty())
                quic_.send_to_many(targets, fwd.data(), fwd.size());
        }
        else if (pkt.packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            forward_video_frame(pkt.session_id, pkt.data.data(), pkt.data.size());
        }
        else if (pkt.packet_type == protocol::STREAM_AUDIO_PACKET_TYPE) {
            forward_stream_audio(pkt);
        }
        else if (pkt.packet_type == protocol::VIDEO_CONTROL_TYPE) {
            handle_video_control(pkt);
        }
    }
}

void Server::send_error(uint32_t session_id, const std::string& message) {
    BinaryWriter writer;
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

void Server::handle_message(const IncomingMessage& msg) {
	ZoneScopedN("Server::handle_message");
    auto session = quic_.get_session(msg.session_id);
    if (!session) return;

    switch (msg.type) {

    // ── Authentication (Ed25519 identity) ──────────────────────────────
    case protocol::ControlMessageType::AUTH_IDENTITY: {
        if (session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());

        // Read public key (32 bytes)
        PublicKey pubkey{};
        reader.read_bytes(pubkey.data(), 32);
        std::string display_name = reader.read_string();
        uint64_t timestamp = reader.read_u64();
        Signature sig{};
        reader.read_bytes(sig.data(), 64);
        if (reader.error()) {
            send_error(msg.session_id, "Malformed auth message");
            break;
        }

        // Verify timestamp is within ±60 seconds
        auto now = static_cast<uint64_t>(std::time(nullptr));
        int64_t diff = static_cast<int64_t>(now) - static_cast<int64_t>(timestamp);
        if (diff > 60 || diff < -60) {
            send_error(msg.session_id, "Auth timestamp out of range");
            break;
        }

        // Reconstruct signed message: pubkey(32) + display_name + timestamp(8)
        BinaryWriter sig_msg;
        sig_msg.write_bytes(pubkey.data(), 32);
        sig_msg.write_string(display_name);
        sig_msg.write_u64(timestamp);

        // Verify Ed25519 signature
        if (!parties::ed25519_verify(sig_msg.data().data(), sig_msg.data().size(),
                                      sig, pubkey)) {
            send_error(msg.session_id, "Invalid signature");
            break;
        }

        Fingerprint fp = parties::public_key_fingerprint(pubkey);
        std::printf("[Server] Auth from session %u: name='%s' fp=%s\n",
                    msg.session_id, display_name.c_str(), fp.c_str());

        // Look up or auto-create user
        auto user = db_.get_user_by_pubkey(pubkey);
        if (!user) {
            // Auto-create new user
            Role initial_role = Role::User;

            // Check if this fingerprint is a root user
            for (const auto& root_fp : config_.root_fingerprints) {
                if (root_fp == fp) {
                    initial_role = Role::Owner;
                    std::printf("[Server] Root fingerprint matched — granting Owner role\n");
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
            std::printf("[Server] New identity registered: '%s' (id=%u)\n",
                        display_name.c_str(), user->id);
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
                    std::printf("[Server] Promoted user '%s' to Owner (root fingerprint)\n",
                                display_name.c_str());
                    break;
                }
            }
        }

        // Auth success
        session->authenticated = true;
        session->user_id = user->id;
        session->username = user->display_name;
        session->role = user->role;
        session->public_key = pubkey;

        db_.update_last_login(user->id);

        // Generate session token
        parties::random_bytes(session->session_token.data(), session->session_token.size());

        // Send AUTH_RESPONSE: [user_id][session_token(32)][role][server_name]
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_bytes(session->session_token.data(), session->session_token.size());
        writer.write_u8(static_cast<uint8_t>(session->role));
        writer.write_string(config_.server_name);

        quic_.send_to(msg.session_id, protocol::ControlMessageType::AUTH_RESPONSE,
                     writer.data().data(), writer.data().size());

        // Send channel list immediately after auth
        send_channel_list(msg.session_id);

        std::printf("[Server] User '%s' (id=%u, role=%d) authenticated\n",
                    user->display_name.c_str(), user->id, user->role);
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
            session->channel_id = 0;
            BinaryWriter leave_writer;
            leave_writer.write_u32(session->user_id);
            leave_writer.write_u32(old_channel);
            auto all = quic_.get_sessions();
            for (auto& s : all) {
                if (s->authenticated && s->channel_id == old_channel)
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                                   leave_writer.data().data(), leave_writer.data().size());
            }
        }

        session->channel_id = channel_id;
        std::printf("[Server] User '%s' joined channel '%s' (%u)\n",
                    session->username.c_str(), channel->name.c_str(), channel_id);

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

        // Send channel encryption key
        send_channel_key(msg.session_id, channel_id);

        // Notify new joiner about all active screen sharers in this channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto ss_it = channel_screen_sharers_.find(channel_id);
            if (ss_it != channel_screen_sharers_.end()) {
                auto all2 = quic_.get_sessions();
                for (UserId sharer_id : ss_it->second) {
                    for (auto& s : all2) {
                        if (s->user_id == sharer_id && s->authenticated) {
                            BinaryWriter ss_writer;
                            ss_writer.write_u32(s->user_id);
                            ss_writer.write_u8(s->share_codec);
                            ss_writer.write_u16(s->share_width);
                            ss_writer.write_u16(s->share_height);
                            quic_.send_to(msg.session_id,
                                         protocol::ControlMessageType::SCREEN_SHARE_STARTED,
                                         ss_writer.data().data(), ss_writer.data().size());
                            break;
                        }
                    }
                }
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
                if (s->id != msg.session_id &&
                    s->authenticated &&
                    s->channel_id == channel_id) {
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_JOINED_CHANNEL,
                                   join_writer.data().data(), join_writer.data().size());
                }
            }
        }
        break;
    }

    // ── Channel leave ───────────────────────────────────────────────────
    case protocol::ControlMessageType::CHANNEL_LEAVE: {
        if (!session->authenticated || session->channel_id == 0) break;

        ChannelId old_channel = session->channel_id;
        stop_screen_share(old_channel, session->user_id);
        session->subscribed_sharer = 0;
        session->channel_id = 0;

        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u32(old_channel);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == old_channel)
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

        std::printf("[Server] Channel '%s' created by '%s'\n",
                    name.c_str(), session->username.c_str());

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

        // Kick everyone from the channel first
        auto all = quic_.get_sessions();
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
            send_error(msg.session_id, "Permission denied");
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
                    send_error(msg.session_id, "Cannot kick a user with equal or higher role");
                    break;
                }
                send_error(s->id, "You have been kicked from the server");
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

        // Notify all in channel (including sender for confirmation)
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u8(codec_id);
        writer.write_u16(width);
        writer.write_u16(height);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == ch) {
                quic_.send_to(s->id, protocol::ControlMessageType::SCREEN_SHARE_STARTED,
                               writer.data().data(), writer.data().size());
            }
        }

        std::printf("[Server] User '%s' started screen sharing in channel %u (%ux%u)\n",
                    session->username.c_str(), ch, width, height);
        break;
    }

    // ── Screen share stop ────────────────────────────────────────────────
    case protocol::ControlMessageType::SCREEN_SHARE_STOP: {
        if (!session->authenticated || session->channel_id == 0) break;
        stop_screen_share(session->channel_id, session->user_id);
        break;
    }

    // ── Screen share view (subscribe/unsubscribe) ────────────────────────
    case protocol::ControlMessageType::SCREEN_SHARE_VIEW: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t target_id = reader.read_u32();
        if (reader.error()) break;

        if (target_id == 0) {
            session->subscribed_sharer = 0;
        } else {
            // Verify target is actually sharing in this channel
            bool is_sharer = false;
            {
                std::lock_guard<std::mutex> lock(sharers_mutex_);
                auto it = channel_screen_sharers_.find(session->channel_id);
                is_sharer = (it != channel_screen_sharers_.end() &&
                             it->second.count(target_id));
            }
            if (is_sharer) {
                session->subscribed_sharer = target_id;

                // Auto-PLI: tell the sharer to send a keyframe so the new viewer
                // can decode from the Sequence Header
                auto all = quic_.get_sessions();
                for (auto& s : all) {
                    if (s->user_id == target_id && s->authenticated) {
                        std::vector<uint8_t> pli;
                        pli.push_back(protocol::VIDEO_CONTROL_TYPE);
                        pli.push_back(protocol::VIDEO_CTL_PLI);
                        uint32_t requester_id = session->user_id;
                        pli.insert(pli.end(), reinterpret_cast<uint8_t*>(&requester_id),
                                   reinterpret_cast<uint8_t*>(&requester_id) + 4);
                        quic_.send_datagram(s->id, pli.data(), pli.size());
                        break;
                    }
                }
            }
        }
        break;
    }

    default:
        std::fprintf(stderr, "[Server] Unhandled message type 0x%04X from session %u\n",
                     static_cast<unsigned>(msg.type), msg.session_id);
        break;
    }
}

void Server::on_client_disconnect(uint32_t session_id) {
	ZoneScopedN("Server::on_client_disconnect");
    auto session = quic_.get_session(session_id);
    if (session && session->authenticated && session->channel_id != 0) {
        // Clean up screen share if this user was sharing
        stop_screen_share(session->channel_id, session->user_id);
        session->subscribed_sharer = 0;

        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u32(session->channel_id);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->id != session_id &&
                s->authenticated &&
                s->channel_id == session->channel_id) {
                quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                               writer.data().data(), writer.data().size());
            }
        }
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

    // Reconstruct forwarded packet: [type(1)][sender_id(4)][data]
    size_t fwd_len = 1 + 4 + len;
    auto* fwd = new uint8_t[fwd_len];
    fwd[0] = protocol::VIDEO_FRAME_PACKET_TYPE;
    uint32_t uid = session->user_id;
    std::memcpy(fwd + 1, &uid, 4);
    std::memcpy(fwd + 5, data, len);

    // Forward to viewers subscribed to this sharer via video stream
    auto all_sessions = quic_.get_sessions();
    for (auto& s : all_sessions) {
        if (s->id != session_id &&
            s->authenticated &&
            s->channel_id == session->channel_id &&
            s->subscribed_sharer == session->user_id) {
            quic_.send_video_to(s->id, fwd, fwd_len);
        }
    }
    delete[] fwd;
}

void Server::handle_video_control(const DataPacket& pkt) {
	ZoneScopedN("Server::handle_video_control");
    auto session = quic_.get_session(pkt.session_id);
    if (!session || !session->authenticated || session->channel_id == 0)
        return;

    if (pkt.data.size() < 2) return;
    uint8_t subtype = pkt.data[0];

    if (subtype == protocol::VIDEO_CTL_PLI) {
        // Client sends: [subtype(1)][target_user_id(4)]
        if (pkt.data.size() < 5) return;
        uint32_t target_id;
        std::memcpy(&target_id, pkt.data.data() + 1, 4);

        // Verify target is an active sharer in this channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_screen_sharers_.find(session->channel_id);
            if (it == channel_screen_sharers_.end() ||
                it->second.count(target_id) == 0)
                return;
        }

        // Forward PLI to the target sharer's session
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->user_id == target_id && s->authenticated) {
                std::vector<uint8_t> fwd;
                fwd.push_back(protocol::VIDEO_CONTROL_TYPE);
                fwd.push_back(protocol::VIDEO_CTL_PLI);
                uint32_t requester_id = session->user_id;
                fwd.insert(fwd.end(), reinterpret_cast<uint8_t*>(&requester_id),
                           reinterpret_cast<uint8_t*>(&requester_id) + 4);
                quic_.send_datagram(s->id, fwd.data(), fwd.size());
                break;
            }
        }
    }
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

    // Forward to viewers subscribed to this sharer via datagram
    auto all_sessions = quic_.get_sessions();
    for (auto& s : all_sessions) {
        if (s->id != pkt.session_id &&
            s->authenticated &&
            s->channel_id == session->channel_id &&
            s->subscribed_sharer == session->user_id) {
            quic_.send_datagram(s->id, fwd.data(), fwd.size());
        }
    }
}

void Server::stop_screen_share(ChannelId channel_id, UserId user_id) {
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_screen_sharers_.find(channel_id);
        if (it == channel_screen_sharers_.end()) return;
        if (it->second.erase(user_id) == 0) return; // wasn't sharing
        if (it->second.empty())
            channel_screen_sharers_.erase(it);
    }

    // Clear subscriptions pointing to this sharer
    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->subscribed_sharer == user_id)
            s->subscribed_sharer = 0;
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

    std::printf("[Server] User %u stopped screen sharing in channel %u\n",
                user_id, channel_id);
}

void Server::send_channel_key(uint32_t session_id, ChannelId channel_id) {
	ZoneScopedN("Server::send_channel_key");
    auto it = channel_keys_.find(channel_id);
    if (it == channel_keys_.end()) {
        std::array<uint8_t, 32> key;
        parties::random_bytes(key.data(), key.size());
        channel_keys_[channel_id] = key;
        it = channel_keys_.find(channel_id);
        std::printf("[Server] Generated encryption key for channel %u\n", channel_id);
    }

    BinaryWriter writer;
    writer.write_u32(channel_id);
    writer.write_bytes(it->second.data(), 32);
    quic_.send_to(session_id, protocol::ControlMessageType::CHANNEL_KEY,
                 writer.data().data(), writer.data().size());
}

} // namespace parties::server
