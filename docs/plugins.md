# Parties Server Plugins

Parties supports trusted native server plugins. Plugins are loaded by the server
at startup and can add chat commands, inspect chat messages, create server-owned
bot users, send bot chat, join bots to voice channels, send Opus bot audio, and
query read-only session/user/channel state.

Plugins run in-process. They are not sandboxed. A plugin crash can crash the
server, so only load plugins you trust.

## Feature Scope

Implemented plugin features:

- Native dynamic library loading from a configured plugin directory.
- `plugin.toml` manifests with default-deny server allow-listing.
- Plugin manifest variables passed to `parties_plugin_init`.
- Per-plugin permission grants.
- Lifecycle callbacks.
- Session authenticated/disconnected callbacks.
- Chat command registration and client advertisement.
- Chat command dispatch from normal chat input.
- Chat message observation, rejection, and replacement.
- Server-owned persistent bot users.
- Bot display-name updates.
- Bot chat messages stored and broadcast like normal chat.
- Bot voice join/leave presence.
- Bot Opus packet fan-out through the normal voice path.
- Read-only session, user, voice channel, and text channel lookup APIs.
- Bot voice-channel lookup and move-to-user-voice helper.

Out of scope for the current API:

- Client-side plugins.
- Direct database access.
- Admin mutation APIs.
- Plugin KV storage.
- Timer/scheduler APIs.
- Voice receive/transcription hooks.
- Process isolation or sandboxing.

## Directory Layout

Plugins are discovered by recursively scanning the configured plugin directory
for files named `plugin.toml`.

Example layout:

```text
plugins/
  music_bot/
    plugin.toml
    music_bot.dll
```

Platform library extensions:

| Platform | Extension |
| --- | --- |
| Windows | `.dll` |
| Linux | `.so` |
| macOS | `.dylib` |

## Server Configuration

Plugin loading is disabled by default.

```toml
[plugins]
enabled = true
directory = "plugins"

[[plugins.allow]]
id = "parties.music_bot"
enabled = true
permissions = [
  "read_sessions",
  "read_users",
  "read_channels",
  "read_chat",
  "create_chat_commands",
  "create_bot_users",
  "send_bot_chat",
  "join_bot_voice",
  "send_bot_audio"
]
```

Loading is default-deny:

- The plugin manifest declares requested permissions.
- The server grants permissions through `[[plugins.allow]]`.
- The effective permissions are the intersection of requested and granted
  permissions.
- Missing permissions do not stop the plugin from loading unless plugin init
  depends on the denied host call.

## Plugin Manifest

Each plugin must provide `plugin.toml` next to the library.

```toml
id = "parties.music_bot"
name = "Music Bot"
version = "0.1.0"
api_version = "1.0"
library = "music_bot.dll"

variables = {
  youtube_api_key = "env:YOUTUBE_API_KEY",
  default_playlist = "favorites"
}

permissions = [
  "read_sessions",
  "read_users",
  "read_channels",
  "create_chat_commands",
  "create_bot_users",
  "send_bot_chat",
  "join_bot_voice",
  "send_bot_audio"
]
```

Rules:

- `id` must match the server allow-list entry.
- `api_version` must currently be `"1.0"`.
- `library` is resolved relative to the manifest directory.
- `variables` is optional. Keys and values must be strings.
- Unsupported manifests are skipped; one bad plugin does not stop other plugins
  from loading.

Variables may also be written as a TOML table:

```toml
[variables]
youtube_api_key = "env-or-secret-name"
default_playlist = "favorites"
```

The server passes variables to `parties_plugin_init` as immutable key/value
pairs in `Host`. They are plugin-local configuration, not permissions or
secrets management. Do not put long-lived secrets in world-readable manifests.

If a variable value starts with `env:`, the server resolves the rest of the
value as an environment variable at plugin load time:

```toml
variables = { youtube_api_key = "env:YOUTUBE_API_KEY" }
```

The plugin receives the resolved environment value, not the literal `env:...`
string. If the environment variable is missing, the plugin manifest is rejected
and the plugin is not loaded.

## Permissions

| Permission | Allows |
| --- | --- |
| `read_sessions` | Read session snapshots, resolve a user's voice channel, move a bot to a user's voice channel. |
| `read_users` | Read basic user records and resolve a user by display name. |
| `read_channels` | Read voice/text channel metadata and channel lists. |
| `read_chat` | Observe chat messages with `on_chat_message`. |
| `moderate_chat` | Reject or replace user chat messages. |
| `create_chat_commands` | Register slash-style chat commands. |
| `create_bot_users` | Create, destroy, and rename server-owned bot users. |
| `send_bot_chat` | Send stored chat messages as bot users. |
| `join_bot_voice` | Join, leave, and move bot users in voice channels. |
| `send_bot_audio` | Send Opus packets as bot users. |

Permissions are policy checks, not a sandbox.

## ABI Rules

The public header is:

```text
sdk/include/parties/plugin_api.h
```

All ABI structs begin with:

```cpp
struct AbiHeader {
    uint32_t size;
    uint16_t api_major;
    uint16_t api_minor;
};
```

Initialize ABI structs with:

```cpp
parties::plugin::make_abi_header<T>()
```

Host function pointers are appended to `Host` over time. Plugins should check
both the function pointer and `host->abi.size` before using a newer field when
they need to run against older servers.

Required export:

```cpp
extern "C" bool parties_plugin_init(const parties::plugin::Host* host,
                                    parties::plugin::Registration* registration);
```

Optional export:

```cpp
extern "C" void parties_plugin_shutdown();
```

The server owns the `Host::context` pointer. Plugins may copy the `Host` table
for later synchronous host calls. Manifest variable pointers in `Host` remain
valid for the plugin lifetime.

## Handles

```cpp
using SessionId = uint32_t;
using UserId = uint32_t;
using ChannelId = uint32_t;
using MessageId = uint64_t;

struct Bot;
using BotHandle = Bot*;
```

`SessionId`, `UserId`, `ChannelId`, and `MessageId` are scalar identifiers.
`BotHandle` is an opaque server-owned handle. Plugins must pass `BotHandle` back
to the host but must not dereference it or persist it across server runs.

## Snapshot Structs

String fields are fixed-size buffers owned by the output struct. Long strings
are truncated and always null-terminated.

```cpp
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
```

For text channels, `user_count` and `max_users` are currently `0`.

## Host API

The server passes a `Host` table to `parties_plugin_init`.

### Manifest Variables

```cpp
struct PluginVariable {
    AbiHeader abi;
    const char* key;
    const char* value;
};

struct Host {
    // ...
    const PluginVariable* variables;
    size_t variable_count;
};
```

Variables come from `plugin.toml` and are available during
`parties_plugin_init`. Values are strings exactly as configured. Plugins should
copy values they need after shutdown begins.

### Logging And Time

```cpp
void (*log)(void* context, uint8_t level, const char* message);
uint64_t (*now_ms)(void* context);
```

`log` writes through the server logger with the plugin id attached.

`now_ms` returns a monotonic millisecond timestamp.

### Chat Commands

```cpp
bool (*create_chat_commands)(void* context,
                             const CommandDefinition* commands,
                             size_t command_count);
```

Requires `create_chat_commands`.

Rules:

- Command names do not include the leading slash.
- Names must be non-empty, at most 64 bytes, and contain only ASCII
  alphanumeric characters, `_`, or `-`.
- Command names are globally unique across loaded plugins.
- Registered commands are sent to clients after authentication.

### Bot Users

```cpp
bool (*create_bot_user)(void* context,
                        const char* key,
                        const char* display_name,
                        BotHandle* out_bot,
                        UserId* out_user_id);

bool (*destroy_bot_user)(void* context, BotHandle bot);

bool (*set_bot_display_name)(void* context,
                             BotHandle bot,
                             const char* display_name);
```

Requires `create_bot_users`.

Bot identity is stable by `(plugin_id, key)`. Calling `create_bot_user` again
with the same key reuses the existing persisted synthetic user row. Bot users
are stored in `users` with `is_bot`, `bot_owner_plugin`, and `bot_key` metadata.

Destroying a bot invalidates the runtime `BotHandle`, but it does not delete the
persisted user row.

### Bot Chat

```cpp
bool (*send_bot_chat)(void* context,
                      BotHandle bot,
                      ChannelId text_channel_id,
                      const char* text,
                      MessageId* out_message_id);
```

Requires `send_bot_chat`.

Bot chat:

- validates the target text channel,
- follows normal message length limits,
- stores the message in the database,
- broadcasts the normal `CHAT_MESSAGE` payload,
- does not recursively invoke `on_chat_message`.

### Bot Voice

```cpp
bool (*join_bot_voice)(void* context,
                       BotHandle bot,
                       ChannelId voice_channel_id);

bool (*leave_bot_voice)(void* context, BotHandle bot);

bool (*send_bot_voice_packet)(void* context,
                              BotHandle bot,
                              uint16_t sequence,
                              const uint8_t* opus_payload,
                              size_t opus_payload_len);
```

`join_bot_voice` and `leave_bot_voice` require `join_bot_voice`.

`send_bot_voice_packet` requires `send_bot_audio`.

Voice packet expectations:

- Payload is Opus, not PCM.
- Sample rate should be 48 kHz.
- Frame duration should match normal voice pacing, currently 20 ms.
- The plugin provides only the Opus payload and sequence number.
- The server builds and forwards the normal voice packet shape:

```text
VOICE_PACKET_TYPE u8
sender_user_id u32
sequence u16
opus_payload bytes
```

Audio is forwarded to authenticated users in the same voice channel who are not
deafened.

### Session And User Lookup

```cpp
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
```

Permissions:

- `user_voice_channel` requires `read_sessions`.
- `get_session_info` requires `read_sessions`.
- `get_user_info` requires `read_users`.
- `find_user_by_name` requires `read_users`.

`user_voice_channel` returns `true` when the user id is known to the live server,
including server-owned bots. It writes `0` when the known user is not currently
in voice. It returns `false` for invalid arguments, missing permission, or an
unknown/offline user id.

`get_session_info` only returns live session state. Bot users do not have
sessions.

`get_user_info` reads persisted users and can return offline users.

`find_user_by_name` currently matches display names exactly.

### Channel Lookup

```cpp
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
```

Requires `read_channels`.

List APIs use a two-pass pattern:

```cpp
size_t count = 0;
host->list_voice_channels(host->context, nullptr, &count);

std::vector<parties::plugin::ChannelInfo> channels(count);
size_t capacity = channels.size();
bool complete = host->list_voice_channels(host->context,
                                          channels.data(),
                                          &capacity);
```

On return, `inout_count` is set to the total number of channels. If the provided
buffer is too small, the host copies as many entries as fit and returns `false`.

### Bot Voice Helpers

```cpp
bool (*bot_voice_channel)(void* context,
                          BotHandle bot,
                          ChannelId* out_voice_channel_id);

bool (*move_bot_to_user_voice)(void* context,
                               BotHandle bot,
                               UserId user_id);
```

`bot_voice_channel` reads a bot owned by the calling plugin. It writes `0` when
the bot is not currently in voice.

`move_bot_to_user_voice` requires both `read_sessions` and `join_bot_voice`. It
resolves the target user's current voice channel, fails if the user is unknown or
not in voice, and then joins the bot to that voice channel.

## Registration API

Plugins fill a `Registration` table during init.

```cpp
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
```

Callbacks are optional. Null callbacks are ignored.

Callback timing:

- `on_server_started` runs after the server starts and plugins load.
- `on_server_stopping` runs while the plugin manager shuts down.
- `on_session_authenticated` runs after a client authenticates.
- `on_session_disconnected` runs during disconnect cleanup on the server main
  loop.
- `on_chat_message` runs before user chat is stored or broadcast.
- `on_chat_command` runs when a registered slash command is invoked.

## Chat Message Moderation

```cpp
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
```

Rules:

- `read_chat` allows observation.
- `moderate_chat` is required for `Reject` and `ReplaceText`.
- Plugins run in load order.
- If a plugin replaces text, later plugins see the updated text.
- If a plugin rejects text, dispatch stops and the sender receives an error.
- Pointers in `ChatMessage` are valid only during the callback.

## Chat Commands

```cpp
struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
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

struct ChatCommandInvocation {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    const char* command_name;
    const char* args;
    const char* raw_text;
    const CommandArgumentValue* parsed_args;
    size_t parsed_arg_count;
};
```

`usage` is also the command argument schema. The server parses it when the plugin
registers commands, rejects invalid schemas, and parses user input before calling
`on_chat_command`.

Schema syntax:

- The first token must be the command name with a slash: `/restart-audio-receiver`.
- Required arguments use braces: `{userId:uint32}`.
- Optional arguments use brackets: `[reason:string]`.
- Argument names may contain ASCII letters, digits, and `_`.
- Required arguments must not appear after optional arguments.
- `string...` captures the rest of the line and must be the final argument.
- Old display-only placeholders such as `[text]` or `<url>` are invalid.

Supported argument types:

| Type | Parsed field |
| --- | --- |
| `bool` | `bool_value`, accepted values are `true`, `false`, `1`, `0` |
| `int8`, `int16`, `int32`, `int64` | `i64_value` |
| `uint8`, `uint16`, `uint32`, `uint64` | `u64_value` |
| `float`, `double` | `f64_value` |
| `string` | `string_value` |
| `string...` | `string_value` |

Examples:

```cpp
command.name = "restart-audio-receiver";
command.usage = "/restart-audio-receiver {userId:uint32}";
```

```cpp
command.name = "play";
command.usage = "/play {url:string} [announce:bool]";
```

```cpp
command.name = "say";
command.usage = "/say [text:string...]";
```

Command invocation rules:

- A user sends normal chat text such as `/play https://example`.
- If the first token matches a registered command, the server dispatches the
  command to the owning plugin.
- Registered command invocations are not stored as normal chat.
- Unknown slash commands return a server error.
- Missing required arguments, invalid typed values, and extra arguments return a
  server error before the plugin callback runs.
- `args` contains the raw argument text after the command name.
- `parsed_args` contains one entry per schema argument. Optional missing
  arguments are present with `present == 0`.
- Pointers in `ChatCommandInvocation` are valid only during the callback.

## Minimal Command Plugin

```cpp
#include <parties/plugin_api.h>

#include <cstring>

#ifdef _WIN32
#define PARTIES_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PARTIES_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

parties::plugin::Host g_host{};

void on_chat_command(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!invocation || !invocation->command_name)
        return;

    if (std::strcmp(invocation->command_name, "hello") == 0 && g_host.log) {
        g_host.log(g_host.context,
                   static_cast<uint8_t>(parties::plugin::LogLevel::Info),
                   "hello command invoked");
    }
}

} // namespace

PARTIES_PLUGIN_EXPORT bool parties_plugin_init(
    const parties::plugin::Host* host,
    parties::plugin::Registration* registration) {
    if (!host || !registration || !host->create_chat_commands)
        return false;

    g_host = *host;

    parties::plugin::CommandDefinition command{};
    command.abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    command.name = "hello";
    command.description = "Log a hello message.";
    command.usage = "/hello";

    if (!g_host.create_chat_commands(g_host.context, &command, 1))
        return false;

    registration->abi = parties::plugin::make_abi_header<parties::plugin::Registration>();
    registration->on_chat_command = &on_chat_command;
    return true;
}
```

Required manifest/server permission:

```toml
"create_chat_commands"
```

## Bot Chat Example

```cpp
parties::plugin::BotHandle bot = nullptr;
parties::plugin::UserId bot_user_id = 0;

bool ensure_bot() {
    if (bot)
        return true;
    return g_host.create_bot_user &&
           g_host.create_bot_user(g_host.context,
                                  "default",
                                  "Music Bot",
                                  &bot,
                                  &bot_user_id);
}

void send_reply(parties::plugin::ChannelId text_channel_id, const char* text) {
    if (!ensure_bot() || !g_host.send_bot_chat)
        return;

    parties::plugin::MessageId message_id = 0;
    g_host.send_bot_chat(g_host.context, bot, text_channel_id, text, &message_id);
}
```

Required manifest/server permissions:

```toml
"create_bot_users",
"send_bot_chat"
```

## Move Bot To Caller Voice Example

```cpp
void join_caller(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!ensure_bot() || !g_host.move_bot_to_user_voice)
        return;

    if (!g_host.move_bot_to_user_voice(g_host.context, bot, invocation->user_id)) {
        send_reply(invocation->text_channel_id, "Join a voice channel first.");
        return;
    }

    send_reply(invocation->text_channel_id, "Joined your voice channel.");
}
```

Required manifest/server permissions:

```toml
"read_sessions",
"create_bot_users",
"send_bot_chat",
"join_bot_voice"
```

## Send Bot Opus Audio Example

```cpp
void send_silence_frame() {
    if (!bot || !g_host.send_bot_voice_packet)
        return;

    static uint16_t sequence = 0;

    // Opus comfort-noise/silence payload used by the test plugin. Real plugins
    // should encode actual 48 kHz Opus frames.
    const uint8_t opus_payload[] = {0xf8, 0xff, 0xfe};

    g_host.send_bot_voice_packet(g_host.context,
                                 bot,
                                 sequence++,
                                 opus_payload,
                                 sizeof(opus_payload));
}
```

Required manifest/server permissions:

```toml
"join_bot_voice",
"send_bot_audio"
```

## Lookup Example

```cpp
void describe_user(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!g_host.get_user_info || !g_host.user_voice_channel)
        return;

    parties::plugin::UserInfo user{};
    if (!g_host.get_user_info(g_host.context, invocation->user_id, &user))
        return;

    parties::plugin::ChannelId voice_channel = 0;
    const bool known_live_user =
        g_host.user_voice_channel(g_host.context, invocation->user_id, &voice_channel);

    char message[256];
    std::snprintf(message, sizeof(message),
                  "%s is %s voice channel %u",
                  user.display_name,
                  known_live_user && voice_channel != 0 ? "in" : "not in",
                  voice_channel);

    send_reply(invocation->text_channel_id, message);
}
```

Required manifest/server permissions:

```toml
"read_users",
"read_sessions",
"send_bot_chat"
```

## Threading Model

Plugin lifecycle, chat, command, and session callbacks are dispatched from the
server main loop.

Host calls are synchronous and may be called from plugin-owned worker threads.
When a worker thread calls a host function that touches server state, the server
marshals that request onto the server main loop and blocks the caller until the
result is available. Calls made from server-dispatched plugin callbacks execute
inline on the main loop.

Plugins are still responsible for their own memory and thread ownership:

- Do not unload or free plugin-owned data while a worker thread might still use
  it.
- Stop and join plugin-owned worker threads from `parties_plugin_shutdown`.
- Treat host calls as blocking calls. Bot audio workers should pace packets and
  avoid unbounded buffering if the server loop is busy.
- Host-provided callback pointers remain valid only for the duration documented
  by the specific callback.

## Failure Behavior

The server handles plugin failures as follows:

- Global plugin loading can be disabled.
- Missing plugin directories log a warning and startup continues.
- Bad manifests, unsupported API versions, denied plugin ids, failed library
  loads, and failed init calls disable only that plugin.
- Missing permissions make host calls fail and log a warning.
- Invalid or duplicate commands are rejected and logged.
- Missing `parties_plugin_shutdown` is allowed.
- Native crashes are process crashes.

## Build Integration

The in-tree example plugin is:

```text
examples/plugins/bot_echo
```

It is built when `BUILD_PLUGIN_EXAMPLES` is enabled and copied under the server
runtime plugin directory. It registers these commands:

- `/botping [text:string...]`
- `/botreset`
- `/botjoin`
- `/botleave`
- `/botvoice`
- `/botapi [displayName:string]`
- `/bottypes {flag:bool} {i8:int8} {u8:uint8} {i16:int16} {u16:uint16} {i32:int32} {u32:uint32} {i64:int64} {u64:uint64} {f:float} {d:double} [note:string...]`
- `/botvars`

`/botapi` is an integration-test command that exercises the read-only lookup
host APIs and bot voice helper APIs through the real plugin boundary.

`/bottypes` is an integration-test command that exercises the typed command
argument parser through the real plugin boundary.

`/botvars` is an integration-test command that echoes a value read from
`plugin.toml` during plugin init.
