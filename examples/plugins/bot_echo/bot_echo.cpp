#include <parties/plugin_api.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define PARTIES_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PARTIES_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

parties::plugin::Host g_host{};
parties::plugin::BotHandle g_bot = nullptr;
parties::plugin::UserId g_bot_user_id = 0;
std::mutex g_bot_mutex;
std::thread g_worker;
std::atomic<bool> g_worker_running{false};
char g_echo_prefix[64] = "unset";

void log_info(const char* message) {
    if (g_host.log)
        g_host.log(g_host.context, static_cast<uint8_t>(parties::plugin::LogLevel::Info), message);
}

bool ensure_bot() {
    std::lock_guard<std::mutex> lock(g_bot_mutex);
    if (g_bot)
        return true;
    if (!g_host.create_bot_user)
        return false;
    return g_host.create_bot_user(g_host.context, "default", "Bot Echo", &g_bot, &g_bot_user_id);
}

void send_bot_text(parties::plugin::ChannelId text_channel_id, const char* text) {
    if (!ensure_bot() || !g_host.send_bot_chat)
        return;
    parties::plugin::MessageId message_id = 0;
    parties::plugin::BotHandle bot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_bot_mutex);
        bot = g_bot;
    }
    g_host.send_bot_chat(g_host.context, bot, text_channel_id, text, &message_id);
}

void send_api_failure(parties::plugin::ChannelId text_channel_id, const char* step) {
    char msg[192];
    std::snprintf(msg, sizeof(msg), "api-fail %s", step);
    send_bot_text(text_channel_id, msg);
}

const char* find_variable(const parties::plugin::Host* host, const char* key) {
    if (!host || !key)
        return nullptr;
    for (size_t i = 0; i < host->variable_count; ++i) {
        const auto& variable = host->variables[i];
        if (variable.key && std::strcmp(variable.key, key) == 0)
            return variable.value;
    }
    return nullptr;
}

const parties::plugin::CommandArgumentValue* find_arg(
    const parties::plugin::ChatCommandInvocation* invocation,
    const char* name) {
    if (!invocation || !name)
        return nullptr;
    for (size_t i = 0; i < invocation->parsed_arg_count; ++i) {
        const auto& arg = invocation->parsed_args[i];
        if (arg.name && std::strcmp(arg.name, name) == 0)
            return &arg;
    }
    return nullptr;
}

template <typename T>
T make_host_out() {
    T value{};
    value.abi = parties::plugin::make_abi_header<T>();
    return value;
}

template <size_t N>
void init_channel_outputs(parties::plugin::ChannelInfo (&channels)[N]) {
    for (auto& channel : channels)
        channel.abi = parties::plugin::make_abi_header<parties::plugin::ChannelInfo>();
}

void on_bottypes(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!ensure_bot())
        return;

    const auto* flag = find_arg(invocation, "flag");
    const auto* i8 = find_arg(invocation, "i8");
    const auto* u8 = find_arg(invocation, "u8");
    const auto* i16 = find_arg(invocation, "i16");
    const auto* u16 = find_arg(invocation, "u16");
    const auto* i32 = find_arg(invocation, "i32");
    const auto* u32 = find_arg(invocation, "u32");
    const auto* i64 = find_arg(invocation, "i64");
    const auto* u64 = find_arg(invocation, "u64");
    const auto* f = find_arg(invocation, "f");
    const auto* d = find_arg(invocation, "d");
    const auto* note = find_arg(invocation, "note");

    if (!flag || !i8 || !u8 || !i16 || !u16 || !i32 || !u32 || !i64 || !u64 || !f || !d ||
        !flag->present || flag->bool_value != 1 ||
        !i8->present || i8->i64_value != -8 ||
        !u8->present || u8->u64_value != 8 ||
        !i16->present || i16->i64_value != -16 ||
        !u16->present || u16->u64_value != 16 ||
        !i32->present || i32->i64_value != -32 ||
        !u32->present || u32->u64_value != 32 ||
        !i64->present || i64->i64_value != -64 ||
        !u64->present || u64->u64_value != 64 ||
        !f->present || f->f64_value != 1.5 ||
        !d->present || d->f64_value != 2.25) {
        send_api_failure(invocation->text_channel_id, "typed_args");
        return;
    }

    const char* note_text = note && note->present && note->string_value
        ? note->string_value
        : "";
    char msg[256];
    std::snprintf(msg, sizeof(msg), "types-ok note=%s", note_text);
    send_bot_text(invocation->text_channel_id, msg);
}

void on_botvars(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!ensure_bot())
        return;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "vars-ok echoPrefix=%s", g_echo_prefix);
    send_bot_text(invocation->text_channel_id, msg);
}

void on_botapi(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!ensure_bot())
        return;

    parties::plugin::ChannelId user_voice = 0;
    if (!g_host.user_voice_channel ||
        !g_host.user_voice_channel(g_host.context, invocation->user_id, &user_voice) ||
        user_voice == 0) {
        send_api_failure(invocation->text_channel_id, "user_voice_channel");
        return;
    }

    auto session = make_host_out<parties::plugin::SessionInfo>();
    if (!g_host.get_session_info ||
        !g_host.get_session_info(g_host.context, invocation->session_id, &session) ||
        session.user_id != invocation->user_id ||
        session.voice_channel_id != user_voice ||
        session.authenticated == 0) {
        send_api_failure(invocation->text_channel_id, "get_session_info");
        return;
    }

    auto user = make_host_out<parties::plugin::UserInfo>();
    if (!g_host.get_user_info ||
        !g_host.get_user_info(g_host.context, invocation->user_id, &user) ||
        user.user_id != invocation->user_id ||
        user.display_name[0] == '\0') {
        send_api_failure(invocation->text_channel_id, "get_user_info");
        return;
    }

    const auto* display_name_arg = find_arg(invocation, "displayName");
    const char* name_to_find = display_name_arg && display_name_arg->present &&
        display_name_arg->string_value && display_name_arg->string_value[0]
        ? display_name_arg->string_value
        : user.display_name;
    parties::plugin::UserId found_user = 0;
    if (!g_host.find_user_by_name ||
        !g_host.find_user_by_name(g_host.context, name_to_find, &found_user) ||
        found_user == 0) {
        send_api_failure(invocation->text_channel_id, "find_user_by_name");
        return;
    }

    auto voice_channel = make_host_out<parties::plugin::ChannelInfo>();
    if (!g_host.get_voice_channel_info ||
        !g_host.get_voice_channel_info(g_host.context, user_voice, &voice_channel) ||
        voice_channel.channel_id != user_voice ||
        voice_channel.name[0] == '\0') {
        send_api_failure(invocation->text_channel_id, "get_voice_channel_info");
        return;
    }

    auto text_channel = make_host_out<parties::plugin::ChannelInfo>();
    if (!g_host.get_text_channel_info ||
        !g_host.get_text_channel_info(g_host.context, invocation->text_channel_id, &text_channel) ||
        text_channel.channel_id != invocation->text_channel_id ||
        text_channel.name[0] == '\0') {
        send_api_failure(invocation->text_channel_id, "get_text_channel_info");
        return;
    }

    size_t voice_count = 0;
    if (!g_host.list_voice_channels ||
        !g_host.list_voice_channels(g_host.context, nullptr, &voice_count) ||
        voice_count == 0) {
        send_api_failure(invocation->text_channel_id, "list_voice_channels_count");
        return;
    }
    parties::plugin::ChannelInfo voice_channels[16]{};
    init_channel_outputs(voice_channels);
    size_t voice_capacity = 16;
    if (!g_host.list_voice_channels(g_host.context, voice_channels, &voice_capacity) ||
        voice_capacity != voice_count) {
        send_api_failure(invocation->text_channel_id, "list_voice_channels");
        return;
    }

    size_t text_count = 0;
    if (!g_host.list_text_channels ||
        !g_host.list_text_channels(g_host.context, nullptr, &text_count) ||
        text_count == 0) {
        send_api_failure(invocation->text_channel_id, "list_text_channels_count");
        return;
    }
    parties::plugin::ChannelInfo text_channels[16]{};
    init_channel_outputs(text_channels);
    size_t text_capacity = 16;
    if (!g_host.list_text_channels(g_host.context, text_channels, &text_capacity) ||
        text_capacity != text_count) {
        send_api_failure(invocation->text_channel_id, "list_text_channels");
        return;
    }

    parties::plugin::ChannelId bot_voice = 0;
    if (!g_host.bot_voice_channel ||
        !g_host.bot_voice_channel(g_host.context, g_bot, &bot_voice)) {
        send_api_failure(invocation->text_channel_id, "bot_voice_channel_before");
        return;
    }

    if (!g_host.move_bot_to_user_voice ||
        !g_host.move_bot_to_user_voice(g_host.context, g_bot, invocation->user_id)) {
        send_api_failure(invocation->text_channel_id, "move_bot_to_user_voice");
        return;
    }

    bot_voice = 0;
    if (!g_host.bot_voice_channel(g_host.context, g_bot, &bot_voice) ||
        bot_voice != user_voice) {
        send_api_failure(invocation->text_channel_id, "bot_voice_channel_after");
        return;
    }

    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "api-ok self=%u found=%u voice=%u text=%u vchannels=%zu tchannels=%zu",
                  invocation->user_id, found_user, user_voice,
                  invocation->text_channel_id, voice_count, text_count);
    send_bot_text(invocation->text_channel_id, msg);
}

void on_chat_command(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!invocation || !invocation->command_name)
        return;
    if (std::strcmp(invocation->command_name, "botping") == 0) {
        if (!ensure_bot() || !g_host.send_bot_chat)
            return;

        const auto* text_arg = find_arg(invocation, "text");
        const char* text = text_arg && text_arg->present &&
            text_arg->string_value && text_arg->string_value[0]
            ? text_arg->string_value
            : "pong";
        parties::plugin::MessageId message_id = 0;
        g_host.send_bot_chat(g_host.context, g_bot, invocation->text_channel_id, text, &message_id);
        return;
    }

    if (std::strcmp(invocation->command_name, "botreset") == 0) {
        parties::plugin::BotHandle bot = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_bot_mutex);
            bot = g_bot;
            g_bot = nullptr;
            g_bot_user_id = 0;
        }
        if (g_host.destroy_bot_user && bot)
            g_host.destroy_bot_user(g_host.context, bot);
        return;
    }

    if (std::strcmp(invocation->command_name, "botjoin") == 0) {
        if (!ensure_bot() || !g_host.join_bot_voice)
            return;
        g_host.join_bot_voice(g_host.context, g_bot, 1);
        return;
    }

    if (std::strcmp(invocation->command_name, "botleave") == 0) {
        if (g_bot && g_host.leave_bot_voice)
            g_host.leave_bot_voice(g_host.context, g_bot);
        return;
    }

    if (std::strcmp(invocation->command_name, "botvoice") == 0) {
        if (!ensure_bot() || !g_host.send_bot_voice_packet)
            return;
        parties::plugin::BotHandle bot = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_bot_mutex);
            bot = g_bot;
        }
        static uint16_t sequence = 0;
        const uint8_t opus_payload[] = {0xf8, 0xff, 0xfe};
        g_host.send_bot_voice_packet(g_host.context, bot, sequence++,
                                     opus_payload, sizeof(opus_payload));
        return;
    }

    if (std::strcmp(invocation->command_name, "botapi") == 0) {
        on_botapi(invocation);
        return;
    }

    if (std::strcmp(invocation->command_name, "bottypes") == 0) {
        on_bottypes(invocation);
        return;
    }

    if (std::strcmp(invocation->command_name, "botvars") == 0) {
        on_botvars(invocation);
        return;
    }

    if (std::strcmp(invocation->command_name, "botworker") == 0) {
        if (!ensure_bot())
            return;
        if (g_worker.joinable() && !g_worker_running)
            g_worker.join();
        if (g_worker.joinable()) {
            send_bot_text(invocation->text_channel_id, "worker-busy");
            return;
        }

        parties::plugin::UserId user_id = invocation->user_id;
        parties::plugin::ChannelId text_channel_id = invocation->text_channel_id;
        g_worker_running = true;
        g_worker = std::thread([user_id, text_channel_id]() {
            parties::plugin::UserInfo user{};
            user.abi = parties::plugin::make_abi_header<parties::plugin::UserInfo>();
            if (!g_host.get_user_info ||
                !g_host.get_user_info(g_host.context, user_id, &user)) {
                send_bot_text(text_channel_id, "worker-fail");
                g_worker_running = false;
                return;
            }
            char msg[192];
            std::snprintf(msg, sizeof(msg), "worker-ok user=%u name=%s",
                          user.user_id, user.display_name);
            send_bot_text(text_channel_id, msg);
            g_worker_running = false;
        });
        return;
    }
}

void on_chat_message(const parties::plugin::ChatMessage* message,
                     parties::plugin::ChatDecision* decision) {
    if (!message || !message->text || !decision)
        return;
    if (std::strcmp(message->text, "bot-moderate-reject") == 0) {
        decision->code = static_cast<uint8_t>(parties::plugin::ChatDecisionCode::Reject);
        decision->rejection_reason = "bot moderation rejected";
        return;
    }
    if (std::strcmp(message->text, "bot-moderate-replace") == 0) {
        decision->code = static_cast<uint8_t>(parties::plugin::ChatDecisionCode::ReplaceText);
        decision->replacement_text = "bot moderation replaced";
        return;
    }
}

} // namespace

PARTIES_PLUGIN_EXPORT bool parties_plugin_init(const parties::plugin::Host* host,
                                               parties::plugin::Registration* registration) {
    if (!host || !registration || !host->create_chat_commands)
        return false;

    g_host = *host;
    if (const char* echo_prefix = find_variable(host, "echo_prefix"))
        std::snprintf(g_echo_prefix, sizeof(g_echo_prefix), "%s", echo_prefix);

    parties::plugin::CommandDefinition commands[9]{};
    commands[0].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[0].name = "botping";
    commands[0].description = "Send a test message as a server bot.";
    commands[0].usage = "/botping [text:string...]";
    commands[1].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[1].name = "botreset";
    commands[1].description = "Drop the runtime bot handle.";
    commands[1].usage = "/botreset";
    commands[2].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[2].name = "botjoin";
    commands[2].description = "Join the test bot to voice channel 1.";
    commands[2].usage = "/botjoin";
    commands[3].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[3].name = "botleave";
    commands[3].description = "Leave the test bot voice channel.";
    commands[3].usage = "/botleave";
    commands[4].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[4].name = "botvoice";
    commands[4].description = "Send one test Opus packet from the bot.";
    commands[4].usage = "/botvoice";
    commands[5].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[5].name = "botapi";
    commands[5].description = "Exercise plugin host lookup APIs.";
    commands[5].usage = "/botapi [displayName:string]";
    commands[6].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[6].name = "bottypes";
    commands[6].description = "Exercise typed command argument parsing.";
    commands[6].usage = "/bottypes {flag:bool} {i8:int8} {u8:uint8} {i16:int16} {u16:uint16} {i32:int32} {u32:uint32} {i64:int64} {u64:uint64} {f:float} {d:double} [note:string...]";
    commands[7].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[7].name = "botvars";
    commands[7].description = "Echo plugin manifest variables.";
    commands[7].usage = "/botvars";
    commands[8].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[8].name = "botworker";
    commands[8].description = "Exercise worker-thread host calls.";
    commands[8].usage = "/botworker";

    if (!g_host.create_chat_commands(g_host.context, commands, 9))
        return false;

    if (const char* mode = find_variable(host, "mode")) {
        if (std::strcmp(mode, "init_false_after_commands") == 0)
            return false;
        if (std::strcmp(mode, "bad_registration_abi") == 0) {
            registration->abi = parties::plugin::make_abi_header<parties::plugin::Registration>();
            registration->abi.api_major = parties::plugin::API_VERSION_MAJOR + 1;
            return true;
        }
    }

    registration->abi = parties::plugin::make_abi_header<parties::plugin::Registration>();
    registration->on_chat_command = &on_chat_command;
    registration->on_chat_message = &on_chat_message;

    log_info("bot_echo initialized");
    return true;
}

PARTIES_PLUGIN_EXPORT void parties_plugin_shutdown() {
    if (g_worker.joinable())
        g_worker.join();
    parties::plugin::BotHandle bot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_bot_mutex);
        bot = g_bot;
        g_bot = nullptr;
        g_bot_user_id = 0;
    }
    if (g_host.destroy_bot_user && bot)
        g_host.destroy_bot_user(g_host.context, bot);
    log_info("bot_echo shutdown");
}
