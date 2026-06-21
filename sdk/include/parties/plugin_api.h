#pragma once

#include <cstddef>
#include <cstdint>

namespace parties::plugin {

constexpr uint16_t API_VERSION_MAJOR = 1;
constexpr uint16_t API_VERSION_MINOR = 0;

struct AbiHeader {
    uint32_t size;
    uint16_t api_major;
    uint16_t api_minor;
};

using SessionId = uint32_t;
using UserId = uint32_t;
using ChannelId = uint32_t;
using MessageId = uint64_t;

struct Bot;
using BotHandle = Bot*;

constexpr size_t MAX_NAME_LEN = 128;
constexpr size_t MAX_FINGERPRINT_LEN = 192;

struct SessionInfo {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId voice_channel_id;
    uint8_t role;
    uint8_t authenticated;
    uint8_t muted;
    uint8_t deafened;
    char username[MAX_NAME_LEN];
};

struct UserInfo {
    AbiHeader abi;
    UserId user_id;
    uint8_t role;
    uint8_t is_bot;
    char display_name[MAX_NAME_LEN];
    char fingerprint[MAX_FINGERPRINT_LEN];
    char bot_owner_plugin[MAX_NAME_LEN];
    char bot_key[MAX_NAME_LEN];
};

struct ChannelInfo {
    AbiHeader abi;
    ChannelId channel_id;
    uint32_t user_count;
    int32_t max_users;
    int32_t sort_order;
    char name[MAX_NAME_LEN];
};

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

enum class CommandArgType : uint8_t {
    String = 0,
    Bool = 1,
    Int8 = 2,
    UInt8 = 3,
    Int16 = 4,
    UInt16 = 5,
    Int32 = 6,
    UInt32 = 7,
    Int64 = 8,
    UInt64 = 9,
    Float = 10,
    Double = 11,
};

struct CommandArgumentValue {
    AbiHeader abi;
    const char* name;
    uint8_t type;
    uint8_t present;
    int64_t i64_value;
    uint64_t u64_value;
    double f64_value;
    uint8_t bool_value;
    const char* string_value;
};

struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
    uint8_t min_role = 3; // Role::User by numeric convention; lower is more privileged.
};

struct PluginVariable {
    AbiHeader abi;
    const char* key;
    const char* value;
};

struct ChatCommandInvocation {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    uint8_t caller_role;
    const char* command_name;
    const char* args;
    const char* raw_text;
    const CommandArgumentValue* parsed_args;
    size_t parsed_arg_count;
};

struct ChatMessage {
    AbiHeader abi;
    SessionId session_id;
    UserId author_user_id;
    ChannelId text_channel_id;
    const char* author_name;
    const char* text;
    uint8_t attachment_count;
};

enum class ChatDecisionCode : uint8_t {
    Continue = 0,
    Reject = 1,
    ReplaceText = 2,
};

struct ChatDecision {
    AbiHeader abi;
    uint8_t code;
    const char* replacement_text;
    const char* rejection_reason;
};

struct Host {
    AbiHeader abi;
    void* context;

    void (*log)(void* context, uint8_t level, const char* message);
    uint64_t (*now_ms)(void* context);

    bool (*create_chat_commands)(void* context,
                                 const CommandDefinition* commands,
                                 size_t command_count);

    bool (*create_bot_user)(void* context,
                            const char* key,
                            const char* display_name,
                            BotHandle* out_bot,
                            UserId* out_user_id);
    bool (*destroy_bot_user)(void* context, BotHandle bot);
    bool (*set_bot_display_name)(void* context,
                                 BotHandle bot,
                                 const char* display_name);

    bool (*send_bot_chat)(void* context,
                          BotHandle bot,
                          ChannelId text_channel_id,
                          const char* text,
                          MessageId* out_message_id);

    bool (*join_bot_voice)(void* context,
                           BotHandle bot,
                           ChannelId voice_channel_id);
    bool (*leave_bot_voice)(void* context, BotHandle bot);
    bool (*send_bot_voice_packet)(void* context,
                                  BotHandle bot,
                                  uint16_t sequence,
                                  const uint8_t* opus_payload,
                                  size_t opus_payload_len);

    bool (*user_voice_channel)(void* context,
                               UserId user_id,
                               ChannelId* out_voice_channel_id);

    bool (*get_session_info)(void* context,
                             SessionId session,
                             SessionInfo* out_info);
    bool (*get_user_info)(void* context,
                          UserId user_id,
                          UserInfo* out_info);
    bool (*find_user_by_name)(void* context,
                              const char* display_name,
                              UserId* out_user_id);

    bool (*get_voice_channel_info)(void* context,
                                   ChannelId channel_id,
                                   ChannelInfo* out_info);
    bool (*get_text_channel_info)(void* context,
                                  ChannelId channel_id,
                                  ChannelInfo* out_info);
    bool (*list_voice_channels)(void* context,
                                ChannelInfo* out_channels,
                                size_t* inout_count);
    bool (*list_text_channels)(void* context,
                               ChannelInfo* out_channels,
                               size_t* inout_count);

    bool (*bot_voice_channel)(void* context,
                              BotHandle bot,
                              ChannelId* out_voice_channel_id);
    bool (*move_bot_to_user_voice)(void* context,
                                   BotHandle bot,
                                   UserId user_id);

    const PluginVariable* variables;
    size_t variable_count;
};

struct Registration {
    AbiHeader abi;

    void (*on_server_started)();
    void (*on_server_stopping)();

    void (*on_session_authenticated)(SessionId session);
    void (*on_session_disconnected)(SessionId session,
                                    UserId user_id,
                                    ChannelId voice_channel_id);

    void (*on_chat_message)(const ChatMessage* message, ChatDecision* decision);
    void (*on_chat_command)(const ChatCommandInvocation* invocation);
};

using InitFn = bool (*)(const Host* host, Registration* registration);
using ShutdownFn = void (*)();

template <typename T>
constexpr AbiHeader make_abi_header() {
    return AbiHeader{static_cast<uint32_t>(sizeof(T)), API_VERSION_MAJOR, API_VERSION_MINOR};
}

} // namespace parties::plugin
