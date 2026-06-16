#pragma once

#include <server/config.h>
#include <parties/plugin_api.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parties::server {

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    struct HostServices {
        struct BotUserResult {
            plugin::UserId user_id = 0;
            bool created = false;
        };

        std::function<std::optional<BotUserResult>(std::string_view plugin_id,
                                                   std::string_view key,
                                                   std::string_view display_name)> create_bot_user;
        std::function<bool(plugin::UserId user_id)> delete_bot_user;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name)> set_bot_display_name;
        std::function<std::optional<plugin::MessageId>(plugin::UserId user_id,
                                                       std::string_view display_name,
                                                       plugin::ChannelId text_channel_id,
                                                       std::string_view text)> send_bot_chat;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name,
                           plugin::ChannelId voice_channel_id)> join_bot_voice;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name,
                           plugin::ChannelId voice_channel_id)> leave_bot_voice;
        std::function<bool(plugin::UserId user_id,
                           plugin::ChannelId voice_channel_id,
                           uint16_t sequence,
                           const uint8_t* opus_payload,
                           size_t opus_payload_len)> send_bot_voice_packet;
        std::function<std::optional<plugin::ChannelId>(plugin::UserId user_id)> user_voice_channel;
        std::function<std::optional<plugin::SessionInfo>(plugin::SessionId session)> get_session_info;
        std::function<std::optional<plugin::UserInfo>(plugin::UserId user_id)> get_user_info;
        std::function<std::optional<plugin::UserId>(std::string_view display_name)> find_user_by_name;
        std::function<std::optional<plugin::ChannelInfo>(plugin::ChannelId channel_id)> get_voice_channel_info;
        std::function<std::optional<plugin::ChannelInfo>(plugin::ChannelId channel_id)> get_text_channel_info;
        std::function<std::vector<plugin::ChannelInfo>()> list_voice_channels;
        std::function<std::vector<plugin::ChannelInfo>()> list_text_channels;
    };

    void set_host_services(HostServices services);

    bool load(const PluginConfig& cfg);
    void shutdown();

    void on_server_started();
    void on_server_stopping();
    void on_session_authenticated(plugin::SessionId session);
    void on_session_disconnected(plugin::SessionId session,
                                 plugin::UserId user_id,
                                 plugin::ChannelId voice_channel_id);

    bool dispatch_chat_command(plugin::SessionId session,
                               plugin::UserId user_id,
                               plugin::ChannelId text_channel_id,
                               uint8_t caller_role,
                               std::string_view command_name,
                               std::string_view args,
                               std::string_view raw_text,
                               std::string* error_message = nullptr);

    enum class ChatResultCode {
        Continue,
        Reject,
    };

    struct ChatResult {
        ChatResultCode code = ChatResultCode::Continue;
        std::string error_message;
    };

    ChatResult process_chat_message(plugin::SessionId session,
                                    plugin::UserId user_id,
                                    plugin::ChannelId text_channel_id,
                                    std::string_view author_name,
                                    std::string& text,
                                    uint8_t attachment_count);

    struct ChatCommand {
        struct Argument {
            std::string name;
            plugin::CommandArgType type = plugin::CommandArgType::String;
            bool required = true;
            bool rest = false;
        };

        std::string plugin_id;
        std::string name;
        std::string description;
        std::string usage;
        uint8_t min_role = 3;
        std::vector<Argument> arguments;
    };

    struct BotVoiceParticipant {
        plugin::UserId user_id = 0;
        plugin::ChannelId voice_channel_id = 0;
        std::string display_name;
    };

    std::vector<ChatCommand> chat_commands() const;
    bool enabled() const { return enabled_; }
    std::vector<BotVoiceParticipant> bot_voice_participants(plugin::ChannelId channel_id = 0) const;
    uint32_t bot_voice_count(plugin::ChannelId channel_id) const;
    bool bot_in_voice_channel(plugin::UserId user_id, plugin::ChannelId channel_id) const;
    void clear_bot_voice_channel(plugin::ChannelId channel_id);

private:
    struct NativeLibrary;
    struct Plugin;
    struct Bot;
    struct PluginGrant;

    static void host_log(void* context, uint8_t level, const char* message);
    static uint64_t host_now_ms(void* context);
    static bool host_create_chat_commands(void* context,
                                          const plugin::CommandDefinition* commands,
                                          size_t command_count);
    static bool host_create_bot_user(void* context,
                                     const char* key,
                                     const char* display_name,
                                     plugin::BotHandle* out_bot,
                                     plugin::UserId* out_user_id);
    static bool host_destroy_bot_user(void* context, plugin::BotHandle bot);
    static bool host_set_bot_display_name(void* context,
                                          plugin::BotHandle bot,
                                          const char* display_name);
    static bool host_send_bot_chat(void* context,
                                   plugin::BotHandle bot,
                                   plugin::ChannelId text_channel_id,
                                   const char* text,
                                   plugin::MessageId* out_message_id);
    static bool host_join_bot_voice(void* context,
                                    plugin::BotHandle bot,
                                    plugin::ChannelId voice_channel_id);
    static bool host_leave_bot_voice(void* context, plugin::BotHandle bot);
    static bool host_send_bot_voice_packet(void* context,
                                           plugin::BotHandle bot,
                                           uint16_t sequence,
                                           const uint8_t* opus_payload,
                                           size_t opus_payload_len);
    static bool host_user_voice_channel(void* context,
                                        plugin::UserId user_id,
                                        plugin::ChannelId* out_voice_channel_id);
    static bool host_get_session_info(void* context,
                                      plugin::SessionId session,
                                      plugin::SessionInfo* out_info);
    static bool host_get_user_info(void* context,
                                   plugin::UserId user_id,
                                   plugin::UserInfo* out_info);
    static bool host_find_user_by_name(void* context,
                                       const char* display_name,
                                       plugin::UserId* out_user_id);
    static bool host_get_voice_channel_info(void* context,
                                            plugin::ChannelId channel_id,
                                            plugin::ChannelInfo* out_info);
    static bool host_get_text_channel_info(void* context,
                                           plugin::ChannelId channel_id,
                                           plugin::ChannelInfo* out_info);
    static bool host_list_voice_channels(void* context,
                                         plugin::ChannelInfo* out_channels,
                                         size_t* inout_count);
    static bool host_list_text_channels(void* context,
                                        plugin::ChannelInfo* out_channels,
                                        size_t* inout_count);
    static bool host_bot_voice_channel(void* context,
                                       plugin::BotHandle bot,
                                       plugin::ChannelId* out_voice_channel_id);
    static bool host_move_bot_to_user_voice(void* context,
                                            plugin::BotHandle bot,
                                            plugin::UserId user_id);

    bool load_manifest(const std::filesystem::path& manifest_path);
    bool register_chat_commands(Plugin& plugin,
                                const plugin::CommandDefinition* commands,
                                size_t command_count);
    void cleanup_plugin_bots(Plugin& plugin, bool delete_users);
    Bot* get_owned_bot(Plugin& plugin, plugin::BotHandle bot) const;
    void reap_destroyed_bots(Plugin& plugin);
    std::optional<plugin::ChannelId> bot_voice_channel(plugin::UserId user_id) const;
    bool check_host_permission(Plugin& plugin, std::string_view permission, std::string_view action) const;
    bool has_permission(const Plugin& plugin, std::string_view permission) const;
    Plugin* find_command_owner(std::string_view command_name) const;
    void disable_plugin(Plugin& plugin, std::string_view reason);
    template <typename Fn>
    bool invoke_plugin_callback(Plugin& plugin, const char* callback_name, Fn&& fn);

    HostServices services_;
    std::unordered_map<std::string, PluginGrant> grants_;
    bool enabled_ = false;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<ChatCommand> chat_commands_;
};

} // namespace parties::server
