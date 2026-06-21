#include <server/server.h>
#include <parties/crypto.h>
#include <parties/audio_common.h>
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
#include <string_view>
#include <thread>

#include <openssl/sha.h>

namespace parties::server {

namespace {
constexpr double CHAT_COMMAND_QUERY_BURST = 5.0;
constexpr double CHAT_COMMAND_QUERY_REFILL_PER_SEC = 2.0;
constexpr auto CHAT_COMMAND_QUERY_PENDING_TTL = std::chrono::seconds(10);
constexpr size_t CHAT_COMMAND_QUERY_MAX_PENDING_PER_SESSION = 64;
constexpr size_t CHAT_COMMAND_QUERY_MAX_RESPONSE_BYTES = 64 * 1024;
}

template <size_t N>
static void copy_plugin_string(char (&dst)[N], std::string_view src) {
    static_assert(N > 0);
    const size_t max_len = N - 1;
    const size_t n = src.size() < max_len ? src.size() : max_len;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

static bool valid_display_name(std::string_view name) {
    if (name.empty() || name.size() >= plugin::MAX_NAME_LEN)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        return uc >= 0x20 && uc != 0x7f;
    });
}

static size_t max_chat_message_length(const Config& config) {
    return config.chat.max_message_length > 0
        ? static_cast<size_t>(config.chat.max_message_length)
        : 0;
}

Server::Server() = default;
Server::~Server() { stop(); }

bool Server::start(const Config& cfg) {
	ZoneScopedN("Server::start");
    server_thread_id_ = std::this_thread::get_id();
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

    // Forward video frames directly from QUIC receive thread,
    // bypassing the polling loop to eliminate up to 1ms latency per frame.
    quic_.on_video_frame = [this](uint32_t session_id, uint8_t packet_type,
                                  const uint8_t* data, size_t len) {
        if (packet_type == protocol::VIDEO_FRAME_PACKET_TYPE) {
            forward_video_frame(session_id, data, len);
        } else if (packet_type == protocol::CAMERA_FRAME_PACKET_TYPE) {
            forward_camera_frame(session_id, data, len);
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

    PluginManager::HostServices plugin_services;
    plugin_services.create_bot_user = [this](std::string_view plugin_id,
                                             std::string_view key,
                                             std::string_view display_name) {
        return invoke_on_server_thread([this,
                                        plugin_id = std::string(plugin_id),
                                        key = std::string(key),
                                        display_name = std::string(display_name)]() {
            return create_plugin_bot_user(plugin_id, key, display_name);
        });
    };
    plugin_services.delete_bot_user = [this](UserId user_id) {
        return invoke_on_server_thread([this, user_id]() {
            return delete_plugin_bot_user(user_id);
        });
    };
    plugin_services.set_bot_display_name = [this](UserId user_id,
                                                  std::string_view display_name) {
        return invoke_on_server_thread([this,
                                        user_id,
                                        display_name = std::string(display_name)]() {
            if (!valid_display_name(display_name) ||
                !bot_display_name_available(display_name, user_id)) {
                return false;
            }
            return db_.update_display_name(user_id, display_name);
        });
    };
    plugin_services.send_bot_chat = [this](UserId user_id,
                                           std::string_view display_name,
                                           ChannelId text_channel_id,
                                           std::string_view text) {
        return invoke_on_server_thread([this,
                                        user_id,
                                        display_name = std::string(display_name),
                                        text_channel_id,
                                        text = std::string(text)]() {
            return store_and_broadcast_chat_message(user_id, display_name, text_channel_id, text);
        });
    };
    plugin_services.join_bot_voice = [this](UserId user_id,
                                            std::string_view display_name,
                                            ChannelId voice_channel_id) {
        return invoke_on_server_thread([this,
                                        user_id,
                                        display_name = std::string(display_name),
                                        voice_channel_id]() {
            return join_plugin_bot_voice(user_id, display_name, voice_channel_id);
        });
    };
    plugin_services.leave_bot_voice = [this](UserId user_id,
                                             std::string_view display_name,
                                             ChannelId voice_channel_id) {
        return invoke_on_server_thread([this,
                                        user_id,
                                        display_name = std::string(display_name),
                                        voice_channel_id]() {
            return leave_plugin_bot_voice(user_id, display_name, voice_channel_id);
        });
    };
    plugin_services.send_bot_voice_packet = [this](UserId user_id,
                                                   ChannelId voice_channel_id,
                                                   uint16_t sequence,
                                                   const uint8_t* opus_payload,
                                                   size_t opus_payload_len) {
        std::vector<uint8_t> payload;
        if (opus_payload && opus_payload_len > 0)
            payload.assign(opus_payload, opus_payload + opus_payload_len);
        return invoke_on_server_thread([this,
                                        user_id,
                                        voice_channel_id,
                                        sequence,
                                        payload = std::move(payload)]() {
            return send_plugin_bot_voice_packet(user_id, voice_channel_id, sequence,
                                                payload.data(), payload.size());
        });
    };
    plugin_services.user_voice_channel = [this](UserId user_id) -> std::optional<ChannelId> {
        return invoke_on_server_thread([this, user_id]() -> std::optional<ChannelId> {
            return quic_.user_voice_channel(user_id);
        });
    };
    plugin_services.get_session_info = [this](plugin::SessionId session_id) -> std::optional<plugin::SessionInfo> {
        return invoke_on_server_thread([this, session_id]() -> std::optional<plugin::SessionInfo> {
        auto session = quic_.session_snapshot(session_id);
        if (!session)
            return std::nullopt;

        plugin::SessionInfo info{};
        info.abi = plugin::make_abi_header<plugin::SessionInfo>();
        info.session_id = session->session_id;
        info.user_id = session->user_id;
        info.voice_channel_id = session->channel_id;
        info.role = static_cast<uint8_t>(session->role);
        info.authenticated = session->authenticated ? 1 : 0;
        info.muted = session->muted ? 1 : 0;
        info.deafened = session->deafened ? 1 : 0;
        copy_plugin_string(info.username, session->username);
        return info;
        });
    };
    plugin_services.get_user_info = [this](plugin::UserId user_id) -> std::optional<plugin::UserInfo> {
        return invoke_on_server_thread([this, user_id]() -> std::optional<plugin::UserInfo> {
        auto user = db_.get_user_by_id(user_id);
        if (!user)
            return std::nullopt;

        plugin::UserInfo info{};
        info.abi = plugin::make_abi_header<plugin::UserInfo>();
        info.user_id = user->id;
        info.role = static_cast<uint8_t>(user->role);
        info.is_bot = user->is_bot ? 1 : 0;
        copy_plugin_string(info.display_name, user->display_name);
        copy_plugin_string(info.fingerprint, user->fingerprint);
        copy_plugin_string(info.bot_owner_plugin, user->bot_owner_plugin);
        copy_plugin_string(info.bot_key, user->bot_key);
        return info;
        });
    };
    plugin_services.find_user_by_name = [this](std::string_view display_name) -> std::optional<plugin::UserId> {
        return invoke_on_server_thread([this,
                                        display_name = std::string(display_name)]() -> std::optional<plugin::UserId> {
        auto users = db_.get_all_users();
        auto it = std::find_if(users.begin(), users.end(), [&](const UserRow& user) {
            return user.display_name == display_name;
        });
        if (it == users.end())
            return std::nullopt;
        return it->id;
        });
    };
    auto voice_user_count = [this](ChannelId channel_id) {
        uint32_t count = plugins_.bot_voice_count(channel_id);
        count += quic_.voice_user_count(channel_id);
        return count;
    };
    plugin_services.get_voice_channel_info = [this, voice_user_count](plugin::ChannelId channel_id)
        -> std::optional<plugin::ChannelInfo> {
        return invoke_on_server_thread([this, voice_user_count, channel_id]()
            -> std::optional<plugin::ChannelInfo> {
        auto channel = db_.get_channel(channel_id);
        if (!channel)
            return std::nullopt;

        plugin::ChannelInfo info{};
        info.abi = plugin::make_abi_header<plugin::ChannelInfo>();
        info.channel_id = channel->id;
        info.user_count = voice_user_count(channel->id);
        info.max_users = channel->max_users;
        info.sort_order = channel->sort_order;
        copy_plugin_string(info.name, channel->name);
        return info;
        });
    };
    plugin_services.get_text_channel_info = [this](plugin::ChannelId channel_id)
        -> std::optional<plugin::ChannelInfo> {
        return invoke_on_server_thread([this, channel_id]() -> std::optional<plugin::ChannelInfo> {
        auto channel = db_.get_text_channel(channel_id);
        if (!channel)
            return std::nullopt;

        plugin::ChannelInfo info{};
        info.abi = plugin::make_abi_header<plugin::ChannelInfo>();
        info.channel_id = channel->id;
        info.sort_order = channel->sort_order;
        copy_plugin_string(info.name, channel->name);
        return info;
        });
    };
    plugin_services.list_voice_channels = [this, voice_user_count]() {
        return invoke_on_server_thread([this, voice_user_count]() {
        std::vector<plugin::ChannelInfo> result;
        auto channels = db_.get_all_channels();
        result.reserve(channels.size());
        for (const auto& channel : channels) {
            plugin::ChannelInfo info{};
            info.abi = plugin::make_abi_header<plugin::ChannelInfo>();
            info.channel_id = channel.id;
            info.user_count = voice_user_count(channel.id);
            info.max_users = channel.max_users;
            info.sort_order = channel.sort_order;
            copy_plugin_string(info.name, channel.name);
            result.push_back(info);
        }
        return result;
        });
    };
    plugin_services.list_text_channels = [this]() {
        return invoke_on_server_thread([this]() {
        std::vector<plugin::ChannelInfo> result;
        auto channels = db_.get_all_text_channels();
        result.reserve(channels.size());
        for (const auto& channel : channels) {
            plugin::ChannelInfo info{};
            info.abi = plugin::make_abi_header<plugin::ChannelInfo>();
            info.channel_id = channel.id;
            info.sort_order = channel.sort_order;
            copy_plugin_string(info.name, channel.name);
            result.push_back(info);
        }
        return result;
        });
    };
    plugin_services.respond_to_command_query = [this](plugin::SessionId session_id,
                                                       uint64_t request_id,
                                                       std::string_view command_name,
                                                       std::string_view argument_name,
                                                       const PluginManager::CommandQueryResponse& response) {
        return invoke_on_server_thread([this,
                                        session_id,
                                        request_id,
                                        command_name = std::string(command_name),
                                        argument_name = std::string(argument_name),
                                        response]() {
            if (!consume_pending_chat_command_query(session_id,
                                                    request_id,
                                                    command_name,
                                                    argument_name,
                                                    std::chrono::steady_clock::now())) {
                return false;
            }
            return send_chat_command_query_response(session_id,
                                                    request_id,
                                                    command_name,
                                                    argument_name,
                                                    response);
        });
    };
    plugins_.set_host_services(std::move(plugin_services));

    if (!plugins_.load(config_.plugins)) {
        LOG_ERROR("Failed to load plugins");
        return false;
    }

    running_ = true;
    plugins_.on_server_started();
    LOG_INFO("{} started successfully", config_.server_name);
    return true;
}

void Server::run() {
    server_thread_id_ = std::this_thread::get_id();
    auto last_retention_check = std::chrono::steady_clock::now();
    uint64_t last_send_failures_ = 0;

    while (running_) {
        ZoneScopedN("Server::run");
        process_plugin_host_calls();
        process_control_messages();
        process_data_packets();
        process_file_transfers();
        process_disconnects();
        process_plugin_host_calls();

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
    if (is_server_thread())
        process_plugin_host_calls();
    plugins_.shutdown();
    quic_.stop();
    db_.close();
    LOG_INFO("Server stopped");
}

void Server::process_plugin_host_calls() {
    auto calls = plugin_host_calls_.drain();
    for (auto& call : calls)
        call();
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
            auto session = quic_.session_snapshot(pkt.session_id);
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

            auto targets = quic_.voice_targets(session->channel_id, pkt.session_id);
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

std::optional<PluginManager::HostServices::BotUserResult> Server::create_plugin_bot_user(
    std::string_view plugin_id,
    std::string_view key,
    std::string_view display_name) {
    if (key.empty() || !valid_display_name(display_name))
        return std::nullopt;

    if (auto existing = db_.get_bot_user(std::string(plugin_id), std::string(key))) {
        if (!bot_display_name_available(display_name, existing->id))
            return std::nullopt;
        if (existing->display_name != display_name)
            db_.update_display_name(existing->id, std::string(display_name));
        if (existing->role != static_cast<int>(Role::Bot))
            db_.set_user_role(existing->id, Role::Bot);
        LOG_INFO("Reusing bot user '{}' (key='{}') for plugin '{}' (user_id={})",
                 display_name, key, plugin_id, existing->id);
        return PluginManager::HostServices::BotUserResult{existing->id, false};
    }

    std::string identity;
    identity.reserve(plugin_id.size() + key.size() + 24);
    identity += "parties-plugin-bot:";
    identity += plugin_id;
    identity.push_back('\0');
    identity += key;

    PublicKey public_key{};
    SHA256(reinterpret_cast<const uint8_t*>(identity.data()), identity.size(), public_key.data());

    if (auto existing = db_.get_user_by_pubkey(public_key)) {
        if (!bot_display_name_available(display_name, existing->id))
            return std::nullopt;
        if (existing->display_name != display_name)
            db_.update_display_name(existing->id, std::string(display_name));
        if (existing->role != static_cast<int>(Role::Bot))
            db_.set_user_role(existing->id, Role::Bot);
        LOG_INFO("Reusing bot user '{}' (key='{}') for plugin '{}' (user_id={})",
                 display_name, key, plugin_id, existing->id);
        return PluginManager::HostServices::BotUserResult{existing->id, false};
    }

    std::string fingerprint = "bot:";
    fingerprint += plugin_id;
    fingerprint += ":";
    fingerprint += public_key_fingerprint(public_key);

    if (!bot_display_name_available(display_name))
        return std::nullopt;

    if (!db_.create_bot_user(public_key, std::string(display_name), fingerprint,
                             std::string(plugin_id), std::string(key), Role::Bot)) {
        if (auto existing = db_.get_bot_user(std::string(plugin_id), std::string(key))) {
            if (existing->role != static_cast<int>(Role::Bot))
                db_.set_user_role(existing->id, Role::Bot);
            LOG_INFO("Reusing bot user '{}' (key='{}') for plugin '{}' (user_id={})",
                     display_name, key, plugin_id, existing->id);
            return PluginManager::HostServices::BotUserResult{existing->id, false};
        }
        LOG_ERROR("Failed to persist bot user '{}' (key='{}') for plugin '{}'",
                  display_name, key, plugin_id);
        return std::nullopt;
    }

    auto user = db_.get_user_by_pubkey(public_key);
    if (!user)
        return std::nullopt;

    return PluginManager::HostServices::BotUserResult{user->id, true};
}

bool Server::delete_plugin_bot_user(UserId user_id) {
    auto user = db_.get_user_by_id(user_id);
    if (!user || !user->is_bot)
        return false;
    return db_.delete_user(user_id);
}

bool Server::bot_display_name_available(std::string_view display_name, UserId self_user_id) {
    auto users = db_.get_all_users();
    return std::none_of(users.begin(), users.end(), [&](const UserRow& user) {
        return user.id != self_user_id && user.display_name == display_name;
    });
}

std::optional<uint64_t> Server::store_and_broadcast_chat_message(UserId sender_id,
                                                                 std::string_view sender_name,
                                                                 uint32_t channel_id,
                                                                 std::string_view text) {
    auto ch = db_.get_text_channel(channel_id);
    if (!ch)
        return std::nullopt;

    if (text.size() > max_chat_message_length(config_))
        return std::nullopt;

    auto now = static_cast<uint64_t>(std::time(nullptr));
    uint64_t msg_id = db_.insert_message(channel_id, sender_id,
                                         std::string(sender_name),
                                         std::string(text), now);
    if (msg_id == 0)
        return std::nullopt;

    BinaryWriter writer;
    writer.write_u64(msg_id);
    writer.write_u32(channel_id);
    writer.write_u32(sender_id);
    writer.write_string(std::string(sender_name));
    writer.write_u64(now);
    writer.write_string(std::string(text));
    writer.write_u8(0); // pinned = false
    writer.write_u8(0); // attachment count

    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->authenticated)
            quic_.send_to(s->id, protocol::ControlMessageType::CHAT_MESSAGE,
                          writer.data().data(), writer.data().size());
    }

    return msg_id;
}

bool Server::join_plugin_bot_voice(UserId user_id,
                                   std::string_view display_name,
                                   ChannelId voice_channel_id) {
    auto channel = db_.get_channel(voice_channel_id);
    if (!channel)
        return false;

    int max = channel->max_users > 0 ? channel->max_users : config_.max_users_per_channel;
    if (max > 0) {
        uint32_t count = plugins_.bot_voice_count(voice_channel_id);
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == voice_channel_id)
                ++count;
        }
        if (static_cast<int>(count) >= max)
            return false;
    }

    BinaryWriter writer;
    writer.write_u32(user_id);
    writer.write_string(std::string(display_name));
    writer.write_u32(voice_channel_id);
    writer.write_u8(static_cast<uint8_t>(Role::Bot));

    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->authenticated)
            quic_.send_to(s->id, protocol::ControlMessageType::USER_JOINED_CHANNEL,
                          writer.data().data(), writer.data().size());
    }

    LOG_INFO("Bot '{}' joined channel '{}' ({})", display_name, channel->name, voice_channel_id);
    return true;
}

bool Server::leave_plugin_bot_voice(UserId user_id,
                                    std::string_view display_name,
                                    ChannelId voice_channel_id) {
    if (voice_channel_id == 0)
        return true;

    BinaryWriter writer;
    writer.write_u32(user_id);
    writer.write_u32(voice_channel_id);

    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->authenticated)
            quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                          writer.data().data(), writer.data().size());
    }

    LOG_INFO("Bot '{}' left channel {}", display_name, voice_channel_id);
    return true;
}

bool Server::send_plugin_bot_voice_packet(UserId user_id,
                                          ChannelId voice_channel_id,
                                          uint16_t sequence,
                                          const uint8_t* opus_payload,
                                          size_t opus_payload_len) {
    if (voice_channel_id == 0 || !opus_payload || opus_payload_len == 0 ||
        opus_payload_len > static_cast<size_t>(audio::MAX_OPUS_PACKET)) {
        return false;
    }
    if (!plugins_.bot_in_voice_channel(user_id, voice_channel_id))
        return false;

    std::vector<uint8_t> fwd;
    fwd.reserve(1 + 4 + 2 + opus_payload_len);
    fwd.push_back(protocol::VOICE_PACKET_TYPE);
    fwd.insert(fwd.end(), reinterpret_cast<uint8_t*>(&user_id),
               reinterpret_cast<uint8_t*>(&user_id) + 4);
    fwd.insert(fwd.end(), reinterpret_cast<uint8_t*>(&sequence),
               reinterpret_cast<uint8_t*>(&sequence) + 2);
    fwd.insert(fwd.end(), opus_payload, opus_payload + opus_payload_len);

    auto targets = quic_.voice_targets(voice_channel_id);
    if (!targets.empty())
        quic_.send_to_many(targets, fwd.data(), fwd.size());

    return true;
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
        user_count += plugins_.bot_voice_count(ch.id);
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

void Server::send_chat_command_list(uint32_t session_id) {
    const auto& commands = plugins_.chat_commands();

    BinaryWriter writer;
    writer.write_u16(static_cast<uint16_t>(std::min<size_t>(commands.size(), UINT16_MAX)));
    size_t count = 0;
    for (const auto& cmd : commands) {
        if (count++ >= UINT16_MAX) break;
        writer.write_string(cmd.name);
        writer.write_string(cmd.description);
        writer.write_string(cmd.usage);
    }

    quic_.send_to(session_id, protocol::ControlMessageType::CHAT_COMMAND_LIST,
                 writer.data().data(), writer.data().size());
}

void Server::send_chat_command_input_list(uint32_t session_id) {
    auto session = quic_.get_session(session_id);
    if (!session ||
        protocol::protocol_minor(session->client_protocol_version) < 1)
        return;

    const auto commands = plugins_.chat_commands();
    size_t command_count = 0;
    for (const auto& cmd : commands) {
        if (!cmd.inputs.empty() && command_count < UINT16_MAX)
            ++command_count;
    }

    BinaryWriter writer;
    writer.write_u16(static_cast<uint16_t>(command_count));
    size_t written_commands = 0;
    for (const auto& cmd : commands) {
        if (cmd.inputs.empty())
            continue;
        if (written_commands++ >= UINT16_MAX)
            break;

        writer.write_string(cmd.name);
        writer.write_u16(static_cast<uint16_t>(std::min<size_t>(cmd.inputs.size(), UINT16_MAX)));
        size_t input_count = 0;
        for (const auto& input : cmd.inputs) {
            if (input_count++ >= UINT16_MAX)
                break;
            writer.write_string(input.argument_name);
            writer.write_u8(static_cast<uint8_t>(input.mode));
            writer.write_u16(input.min_chars);
            writer.write_u16(input.debounce_ms);
            writer.write_u16(input.max_results);
            writer.write_string(input.placeholder);
        }
    }

    quic_.send_to(session_id, protocol::ControlMessageType::CHAT_COMMAND_INPUT_LIST,
                 writer.data().data(), writer.data().size());
}

bool Server::allow_chat_command_query(uint32_t session_id,
                                      std::chrono::steady_clock::time_point now) {
    auto& state = chat_command_query_rate_limits_[session_id];
    if (state.updated_at.time_since_epoch().count() == 0) {
        state.tokens = CHAT_COMMAND_QUERY_BURST;
        state.updated_at = now;
    }

    const auto elapsed = std::chrono::duration<double>(now - state.updated_at).count();
    if (elapsed > 0.0) {
        state.tokens = (std::min)(CHAT_COMMAND_QUERY_BURST,
                                  state.tokens + elapsed * CHAT_COMMAND_QUERY_REFILL_PER_SEC);
        state.updated_at = now;
    }

    if (state.tokens < 1.0)
        return false;

    state.tokens -= 1.0;
    return true;
}

void Server::remember_pending_chat_command_query(
    uint32_t session_id,
    uint64_t request_id,
    std::string_view command_name,
    std::string_view argument_name,
    std::chrono::steady_clock::time_point now) {
    prune_pending_chat_command_queries(now);

    auto& pending = pending_chat_command_queries_[session_id];
    auto existing = std::find_if(pending.begin(), pending.end(),
        [&](const PendingChatCommandQuery& query) {
            return query.request_id == request_id &&
                   query.command_name == command_name &&
                   query.argument_name == argument_name;
        });

    PendingChatCommandQuery entry{
        request_id,
        std::string(command_name),
        std::string(argument_name),
        now + CHAT_COMMAND_QUERY_PENDING_TTL,
    };

    if (existing != pending.end()) {
        *existing = std::move(entry);
        return;
    }

    if (pending.size() >= CHAT_COMMAND_QUERY_MAX_PENDING_PER_SESSION)
        pending.erase(pending.begin());
    pending.push_back(std::move(entry));
}

bool Server::consume_pending_chat_command_query(
    uint32_t session_id,
    uint64_t request_id,
    std::string_view command_name,
    std::string_view argument_name,
    std::chrono::steady_clock::time_point now) {
    prune_pending_chat_command_queries(now);

    auto session_it = pending_chat_command_queries_.find(session_id);
    if (session_it == pending_chat_command_queries_.end())
        return false;

    auto& pending = session_it->second;
    auto query_it = std::find_if(pending.begin(), pending.end(),
        [&](const PendingChatCommandQuery& query) {
            return query.request_id == request_id &&
                   query.command_name == command_name &&
                   query.argument_name == argument_name;
        });
    if (query_it == pending.end())
        return false;

    pending.erase(query_it);
    if (pending.empty())
        pending_chat_command_queries_.erase(session_it);
    return true;
}

void Server::prune_pending_chat_command_queries(std::chrono::steady_clock::time_point now) {
    for (auto session_it = pending_chat_command_queries_.begin();
         session_it != pending_chat_command_queries_.end();) {
        auto& pending = session_it->second;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
            [&](const PendingChatCommandQuery& query) {
                return query.expires_at <= now;
            }), pending.end());

        if (pending.empty())
            session_it = pending_chat_command_queries_.erase(session_it);
        else
            ++session_it;
    }
}

void Server::clear_chat_command_query_state(uint32_t session_id) {
    pending_chat_command_queries_.erase(session_id);
    chat_command_query_rate_limits_.erase(session_id);
}

bool Server::send_chat_command_query_response(
    uint32_t session_id,
    uint64_t request_id,
    std::string_view command_name,
    std::string_view argument_name,
    const PluginManager::CommandQueryResponse& response) {
    auto session = quic_.get_session(session_id);
    if (!session || !session->authenticated)
        return false;

    auto clipped = [](std::string_view value, size_t max_len) {
        return std::string(value.substr(0, max_len));
    };

    BinaryWriter header;
    header.write_u64(request_id);
    header.write_string(clipped(command_name, 64));
    header.write_string(clipped(argument_name, 64));
    header.write_u8(response.status);
    header.write_string(clipped(response.message, 512));

    std::vector<std::vector<uint8_t>> encoded_results;
    encoded_results.reserve(std::min<size_t>(response.results.size(), 25));
    size_t payload_size = header.size() + sizeof(uint16_t);
    for (const auto& result : response.results) {
        if (encoded_results.size() >= 25)
            break;

        BinaryWriter item;
        item.write_string(clipped(result.id, 512));
        item.write_string(clipped(result.title, 512));
        item.write_string(clipped(result.subtitle, 512));
        item.write_string(clipped(result.value, 512));
        item.write_string(clipped(result.kind, 64));
        item.write_u32(result.duration_ms);
        item.write_string(clipped(result.thumbnail_url, 2048));
        if (payload_size + item.size() > CHAT_COMMAND_QUERY_MAX_RESPONSE_BYTES)
            break;

        payload_size += item.size();
        encoded_results.push_back(item.data());
    }

    BinaryWriter writer;
    writer.write_bytes(header.data().data(), header.data().size());
    writer.write_u16(static_cast<uint16_t>(encoded_results.size()));
    for (const auto& item : encoded_results)
        writer.write_bytes(item.data(), item.size());

    return quic_.send_to(session_id, protocol::ControlMessageType::CHAT_COMMAND_QUERY_RESP,
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
        if (!valid_display_name(display_name)) {
            send_error(msg.session_id, "Invalid display name",
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
            if (user->is_bot) {
                send_error(msg.session_id, "Bot users cannot authenticate",
                           protocol::ServerErrorCode::BadAuth);
                break;
            }
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
        session->client_protocol_version = client_version;
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
        send_text_channel_list(msg.session_id);
        send_chat_command_list(msg.session_id);
        send_chat_command_input_list(msg.session_id);
        plugins_.on_session_authenticated(msg.session_id);

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
                auto bots = plugins_.bot_voice_participants(ch.id);
                count += static_cast<uint32_t>(bots.size());
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
                for (const auto& bot : bots) {
                    list_writer.write_u32(bot.user_id);
                    list_writer.write_string(bot.display_name);
                    list_writer.write_u8(static_cast<uint8_t>(Role::Bot));
                    list_writer.write_u8(0);
                    list_writer.write_u8(0);
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
            auto bots = plugins_.bot_voice_participants(channel_id);
            count += static_cast<uint32_t>(bots.size());
            if (static_cast<int>(count) >= max) {
                send_error(msg.session_id, "Channel is full");
                break;
            }
        }

        // Leave current channel if in one
        ChannelId old_channel = session->channel_id;
        if (old_channel != 0 && old_channel != channel_id) {
            stop_screen_share(old_channel, session->user_id);
            stop_camera_share(old_channel, session->user_id);
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
            auto bots = plugins_.bot_voice_participants(channel_id);
            count += static_cast<uint32_t>(bots.size());
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
            for (const auto& bot : bots) {
                list_writer.write_u32(bot.user_id);
                list_writer.write_string(bot.display_name);
                list_writer.write_u8(static_cast<uint8_t>(Role::Bot));
                list_writer.write_u8(0);
                list_writer.write_u8(0);
            }
            quic_.send_to(msg.session_id, protocol::ControlMessageType::CHANNEL_USER_LIST,
                         list_writer.data().data(), list_writer.data().size());
        }

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

        // Notify new joiner about all active camera streamers in this channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto cam_it = channel_camera_sharers_.find(channel_id);
            if (cam_it != channel_camera_sharers_.end()) {
                auto all2 = quic_.get_sessions();
                for (UserId sharer_id : cam_it->second) {
                    for (auto& s : all2) {
                        if (s->user_id == sharer_id && s->authenticated) {
                            BinaryWriter cam_writer;
                            cam_writer.write_u32(s->user_id);
                            cam_writer.write_u8(s->camera_codec);
                            cam_writer.write_u16(s->camera_width);
                            cam_writer.write_u16(s->camera_height);
                            quic_.send_to(msg.session_id,
                                         protocol::ControlMessageType::CAMERA_SHARE_STARTED,
                                         cam_writer.data().data(), cam_writer.data().size());
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
        stop_camera_share(old_channel, session->user_id);
        session->subscribed_sharer = 0;
        session->subscribed_camera = 0;
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
        auto bots = plugins_.bot_voice_participants(channel_id);
        for (const auto& bot : bots) {
            BinaryWriter leave_writer;
            leave_writer.write_u32(bot.user_id);
            leave_writer.write_u32(channel_id);
            for (auto& s : all) {
                if (s->authenticated)
                    quic_.send_to(s->id, protocol::ControlMessageType::USER_LEFT_CHANNEL,
                                  leave_writer.data().data(), leave_writer.data().size());
            }
        }
        plugins_.clear_bot_voice_channel(channel_id);

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

        // Owner role is assigned only via server config; Bot only via plugin bot creation.
        if (target_new_role == Role::Owner) {
            send_error(msg.session_id, "Owner role can only be set in server config");
            break;
        }
        if (target_new_role == Role::Bot) {
            send_error(msg.session_id, "Bot role can only be assigned to plugin bots");
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

    // ── Camera share start ───────────────────────────────────────────────
    case protocol::ControlMessageType::CAMERA_SHARE_START: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint8_t codec_id = reader.read_u8();
        uint16_t width = reader.read_u16();
        uint16_t height = reader.read_u16();
        if (reader.error()) break;

        ChannelId ch = session->channel_id;

        // Allow multiple camera streamers per channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            channel_camera_sharers_[ch].insert(session->user_id);
        }
        session->camera_codec = codec_id;
        session->camera_width = width;
        session->camera_height = height;

        // Notify all in channel (including sender for confirmation)
        BinaryWriter writer;
        writer.write_u32(session->user_id);
        writer.write_u8(codec_id);
        writer.write_u16(width);
        writer.write_u16(height);

        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->authenticated && s->channel_id == ch) {
                quic_.send_to(s->id, protocol::ControlMessageType::CAMERA_SHARE_STARTED,
                               writer.data().data(), writer.data().size());
            }
        }

        LOG_INFO("User '{}' started camera in channel {} ({}x{})", session->username, ch, width, height);
        break;
    }

    // ── Camera share update ──
    case protocol::ControlMessageType::CAMERA_SHARE_UPDATE: {
        if (!session->authenticated || session->channel_id == 0) break;

        // Only accept from active camera streamers
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_camera_sharers_.find(session->channel_id);
            if (it == channel_camera_sharers_.end() ||
                it->second.count(session->user_id) == 0)
                break;
        }

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint8_t codec_id = reader.read_u8();
        uint16_t width = reader.read_u16();
        uint16_t height = reader.read_u16();
        if (reader.error()) break;

        session->camera_codec = codec_id;
        session->camera_width = width;
        session->camera_height = height;

        LOG_INFO("User '{}' camera encoder ready: codec={} {}x{}", session->username, codec_id, width, height);
        break;
    }

    // ── Camera share stop ────────────────────────────────────────────────
    case protocol::ControlMessageType::CAMERA_SHARE_STOP: {
        if (!session->authenticated || session->channel_id == 0) break;
        stop_camera_share(session->channel_id, session->user_id);
        break;
    }

    // ── Camera share view (subscribe/unsubscribe) ────────────────────────
    case protocol::ControlMessageType::CAMERA_SHARE_VIEW: {
        if (!session->authenticated || session->channel_id == 0) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t target_id = reader.read_u32();
        if (reader.error()) break;

        if (target_id == 0) {
            session->subscribed_camera = 0;
        } else {
            // Verify target is actually streaming a camera in this channel
            bool is_sharer = false;
            {
                std::lock_guard<std::mutex> lock(sharers_mutex_);
                auto it = channel_camera_sharers_.find(session->channel_id);
                is_sharer = (it != channel_camera_sharers_.end() &&
                             it->second.count(target_id));
            }
            if (is_sharer) {
                session->subscribed_camera = target_id;

                // Auto-PLI: tell the streamer to send a camera keyframe so the
                // new viewer can decode from the Sequence Header
                auto all = quic_.get_sessions();
                for (auto& s : all) {
                    if (s->user_id == target_id && s->authenticated) {
                        std::vector<uint8_t> pli;
                        pli.push_back(protocol::VIDEO_CONTROL_TYPE);
                        pli.push_back(protocol::VIDEO_CTL_CAMERA_PLI);
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
        if (text.size() > max_chat_message_length(config_)) {
            send_error(msg.session_id, "Message too long");
            break;
        }

        if (text.size() > 1 && text[0] == '/') {
            size_t name_start = 1;
            size_t name_end = text.find_first_of(" \t\r\n", name_start);
            std::string_view command_name(text.data() + name_start,
                (name_end == std::string::npos ? text.size() : name_end) - name_start);
            size_t args_start = name_end == std::string::npos ? text.size() : name_end;
            while (args_start < text.size() &&
                   (text[args_start] == ' ' || text[args_start] == '\t' ||
                    text[args_start] == '\r' || text[args_start] == '\n')) {
                ++args_start;
            }
            std::string_view args(text.data() + args_start, text.size() - args_start);

            if (command_name.empty()) {
                send_error(msg.session_id, "Unknown chat command");
                break;
            }

            std::string command_error;
            if (plugins_.dispatch_chat_command(msg.session_id, session->user_id, channel_id,
                                               static_cast<uint8_t>(session->role),
                                               command_name, args, text, &command_error)) {
                if (!command_error.empty())
                    send_error(msg.session_id, command_error);
                break;
            }

            send_error(msg.session_id, "Unknown chat command");
            break;
        }

        auto chat_result = plugins_.process_chat_message(
            msg.session_id,
            session->user_id,
            channel_id,
            session->username,
            text,
            attachment_count);
        if (chat_result.code == PluginManager::ChatResultCode::Reject) {
            send_error(msg.session_id, chat_result.error_message,
                       protocol::ServerErrorCode::PermissionDenied);
            break;
        }

        if (text.size() > max_chat_message_length(config_)) {
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

    // ── Chat command live query/autocomplete ─────────────────────────
    case protocol::ControlMessageType::CHAT_COMMAND_QUERY: {
        if (!session->authenticated) break;

        BinaryReader reader(msg.payload.data(), msg.payload.size());
        uint32_t channel_id = reader.read_u32();
        uint64_t request_id = reader.read_u64();
        std::string command_name = reader.read_string();
        std::string argument_name = reader.read_string();
        std::string query = reader.read_string();
        uint16_t cursor_pos = reader.read_u16();
        if (reader.error()) break;

        if (command_name.size() > 64 || argument_name.size() > 64 || query.size() > 512) {
            PluginManager::CommandQueryResponse response;
            response.status = static_cast<uint8_t>(plugin::CommandQueryStatus::TooShort);
            response.message = "Invalid command query";
            send_chat_command_query_response(msg.session_id,
                                             request_id,
                                             command_name,
                                             argument_name,
                                             response);
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (!allow_chat_command_query(msg.session_id, now)) {
            PluginManager::CommandQueryResponse response;
            response.status = static_cast<uint8_t>(plugin::CommandQueryStatus::RateLimited);
            response.message = "Too many command queries";
            send_chat_command_query_response(msg.session_id,
                                             request_id,
                                             command_name,
                                             argument_name,
                                             response);
            break;
        }

        auto response = plugins_.dispatch_chat_command_query(
            msg.session_id,
            session->user_id,
            channel_id,
            static_cast<uint8_t>(session->role),
            request_id,
            command_name,
            argument_name,
            query,
            cursor_pos);

        if (response.status != static_cast<uint8_t>(plugin::CommandQueryStatus::Pending)) {
            send_chat_command_query_response(msg.session_id,
                                             request_id,
                                             command_name,
                                             argument_name,
                                             response);
        } else {
            remember_pending_chat_command_query(msg.session_id,
                                                request_id,
                                                command_name,
                                                argument_name,
                                                now);
        }
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
    clear_chat_command_query_state(session_id);
    plugins_.on_session_disconnected(session_id, user_id, channel_id);
    if (channel_id == 0)
        return;

    // Clean up screen share / camera if this user was sharing.
    stop_screen_share(channel_id, user_id);
    stop_camera_share(channel_id, user_id);

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

void Server::forward_camera_frame(uint32_t session_id, const uint8_t* data, size_t len) {
	ZoneScopedN("Server::forward_camera_frame");
    auto session = quic_.get_session(session_id);
    if (!session || !session->authenticated || session->channel_id == 0)
        return;

    // Verify this user is an active camera streamer in the channel
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_camera_sharers_.find(session->channel_id);
        if (it == channel_camera_sharers_.end() ||
            it->second.count(session->user_id) == 0)
            return;
    }

    // Reconstruct forwarded packet: [type(1)][sender_id(4)][data]
    size_t fwd_len = 1 + 4 + len;
    auto* fwd = new uint8_t[fwd_len];
    fwd[0] = protocol::CAMERA_FRAME_PACKET_TYPE;
    uint32_t uid = session->user_id;
    std::memcpy(fwd + 1, &uid, 4);
    std::memcpy(fwd + 5, data, len);

    // Forward to viewers subscribed to this streamer's camera via video stream
    auto all_sessions = quic_.get_sessions();
    for (auto& s : all_sessions) {
        if (s->id != session_id &&
            s->authenticated &&
            s->channel_id == session->channel_id &&
            s->subscribed_camera == session->user_id) {
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
    } else if (subtype == protocol::VIDEO_CTL_CAMERA_PLI) {
        // Client sends: [subtype(1)][target_user_id(4)]
        if (pkt.data.size() < 5) return;
        uint32_t target_id;
        std::memcpy(&target_id, pkt.data.data() + 1, 4);

        // Verify target is an active camera streamer in this channel
        {
            std::lock_guard<std::mutex> lock(sharers_mutex_);
            auto it = channel_camera_sharers_.find(session->channel_id);
            if (it == channel_camera_sharers_.end() ||
                it->second.count(target_id) == 0)
                return;
        }

        // Forward camera PLI to the target streamer's session
        auto all = quic_.get_sessions();
        for (auto& s : all) {
            if (s->user_id == target_id && s->authenticated) {
                std::vector<uint8_t> fwd;
                fwd.push_back(protocol::VIDEO_CONTROL_TYPE);
                fwd.push_back(protocol::VIDEO_CTL_CAMERA_PLI);
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

    LOG_INFO("User {} stopped screen sharing in channel {}", user_id, channel_id);
}

void Server::stop_camera_share(ChannelId channel_id, UserId user_id) {
    {
        std::lock_guard<std::mutex> lock(sharers_mutex_);
        auto it = channel_camera_sharers_.find(channel_id);
        if (it == channel_camera_sharers_.end()) return;
        if (it->second.erase(user_id) == 0) return; // wasn't streaming
        if (it->second.empty())
            channel_camera_sharers_.erase(it);
    }

    // Clear subscriptions pointing to this camera streamer
    auto all = quic_.get_sessions();
    for (auto& s : all) {
        if (s->subscribed_camera == user_id)
            s->subscribed_camera = 0;
    }

    // Notify all in channel
    BinaryWriter writer;
    writer.write_u32(user_id);

    for (auto& s : all) {
        if (s->authenticated && s->channel_id == channel_id) {
            quic_.send_to(s->id, protocol::ControlMessageType::CAMERA_SHARE_STOPPED,
                           writer.data().data(), writer.data().size());
        }
    }

    LOG_INFO("User {} stopped camera in channel {}", user_id, channel_id);
}

} // namespace parties::server
