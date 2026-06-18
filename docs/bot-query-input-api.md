# Bot Query Input API

This document specifies the implemented command live-query API for
Discord-style slash command input, such as `/play <query>`.

The API is split into:

- plugin API v1.1 command input metadata and query callbacks
- protocol v1.1 command input metadata and query response messages
- server-side validation, pending async tracking, rate limiting, and payload caps
- client-side request ids and stale response handling

Client UI rendering and result-panel behavior are outside this spec. The client
API/model may expose query state without implementing an interactive UI.

## Versioning

Plugin API:

```text
API_VERSION_MAJOR = 1
API_VERSION_MINOR = 1
```

Wire protocol:

```text
PROTOCOL_VERSION_MAJOR = 1
PROTOCOL_VERSION_MINOR = 1
```

Existing plugin API 1.0 plugins continue to load when their ABI sizes are
smaller than the v1.1 structs. Existing protocol 1.0 clients continue to receive
the original `CHAT_COMMAND_LIST` shape and do not receive
`CHAT_COMMAND_INPUT_LIST`.

## Model

Live query belongs to a single command argument, not to the command as a whole.
For example:

```text
/play {query:string...}
```

The `query` argument can opt into `LiveQuery`. While the user edits that
argument, the client sends debounced `CHAT_COMMAND_QUERY` requests. The server
validates the request and dispatches it to the plugin that owns the command.
The plugin returns a bounded list of suggestions synchronously, or returns
`Pending` and completes later through `respond_to_command_query`.

Final command execution is unchanged. Pressing Enter sends the existing chat
command text through `CHAT_SEND`; live-query responses are previews only.

## Plugin Command Metadata

Plugins opt in by attaching input metadata to `CommandDefinition`.

```cpp
enum class CommandInputMode : uint8_t {
    None = 0,
    LiveQuery = 1,
};

struct CommandInputDefinition {
    AbiHeader abi;
    const char* argument_name;
    uint8_t mode;
    uint16_t min_chars;
    uint16_t debounce_ms;
    uint16_t max_results;
    const char* placeholder;
};

struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
    uint8_t min_role;
    const CommandInputDefinition* inputs;
    size_t input_count;
};
```

Rules:

- `argument_name` MUST match an argument parsed from `usage`.
- `mode` MUST be `LiveQuery` for server-dispatched query callbacks.
- `min_chars` is enforced by the server.
- `debounce_ms` is advisory metadata for the client.
- `max_results == 0` uses the server default result cap.
- `placeholder` is advisory metadata for the client.
- Older plugins with smaller `CommandDefinition::abi.size` expose no live-query
  inputs.

Example:

```cpp
CommandInputDefinition play_query{};
play_query.abi = make_abi_header<CommandInputDefinition>();
play_query.argument_name = "query";
play_query.mode = static_cast<uint8_t>(CommandInputMode::LiveQuery);
play_query.min_chars = 2;
play_query.debounce_ms = 250;
play_query.max_results = 8;
play_query.placeholder = "Paste a URL or search";

CommandDefinition play{};
play.abi = make_abi_header<CommandDefinition>();
play.name = "play";
play.description = "Play a track in the current voice channel.";
play.usage = "/play {query:string...}";
play.min_role = 3;
play.inputs = &play_query;
play.input_count = 1;

host->create_chat_commands(host->context, &play, 1);
```

## Protocol Messages

All strings use the existing protocol string encoding.

### `CHAT_COMMAND_INPUT_LIST`

Direction: server to client

Message id:

```text
0x050A
```

Payload:

```text
[command_count u16]
repeated command:
  [command_name string]
  [input_count u16]
  repeated input:
    [argument_name string]
    [mode u8]
    [min_chars u16]
    [debounce_ms u16]
    [max_results u16]
    [placeholder string]
```

The server sends this after `CHAT_COMMAND_LIST` only to authenticated clients
whose protocol minor version is at least `1`.

`CHAT_COMMAND_LIST` remains:

```text
[count u16][name string][description string][usage string]...
```

### `CHAT_COMMAND_QUERY`

Direction: client to server

Message id:

```text
0x040C
```

Payload:

```text
[channel_id u32]
[request_id u64]
[command_name string]
[argument_name string]
[query string]
[cursor_pos u16]
```

Fields:

| Field | Meaning |
| --- | --- |
| `channel_id` | Text channel where the command would execute. |
| `request_id` | Client-generated id for matching responses. |
| `command_name` | Command name without `/`. |
| `argument_name` | Live-query argument name. |
| `query` | Current argument text, not the full chat input. |
| `cursor_pos` | Cursor position in UTF-8 bytes inside `query`. |

Client requirements:

- `request_id` MUST change for each new query for an active input.
- The client MUST NOT send a query before `min_chars` is satisfied.
- The client MUST debounce requests using `debounce_ms`.
- The client MUST ignore responses whose `request_id` is not current for the
  active input.

### `CHAT_COMMAND_QUERY_RESP`

Direction: server to client

Message id:

```text
0x050B
```

Payload:

```text
[request_id u64]
[command_name string]
[argument_name string]
[status u8]
[message string]
[result_count u16]
repeated result:
  [id string]
  [title string]
  [subtitle string]
  [value string]
  [kind string]
  [duration_ms u32]
  [thumbnail_url string]
```

Result fields:

| Field | Meaning |
| --- | --- |
| `id` | Stable result id within the provider response. |
| `title` | Main display text. |
| `subtitle` | Secondary display text, such as channel, album, or source. |
| `value` | Value inserted into the command if selected. |
| `kind` | Optional category, such as `url`, `track`, `playlist`, or `user`. |
| `duration_ms` | Optional media duration; `0` means unknown or not applicable. |
| `thumbnail_url` | Optional preview image URL. |

Status values:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `Ok` | Query completed and results are usable. |
| `1` | `NoResults` | Query completed with no matches. |
| `2` | `TooShort` | Query is shorter than `min_chars` or invalid. |
| `3` | `RateLimited` | Client is querying too quickly. |
| `4` | `PluginError` | Plugin failed but command input can continue. |
| `5` | `PermissionDenied` | User cannot query this command. |
| `6` | `Pending` | Plugin accepted the query asynchronously. This status is internal to plugin callback flow and MUST NOT be sent as a final client response. |

## Plugin Query Callback

Plugins implement live-query responses by setting
`Registration::on_chat_command_query`.

```cpp
struct CommandQueryRequest {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    uint8_t caller_role;
    uint64_t request_id;
    const char* command_name;
    const char* argument_name;
    const char* query;
    uint16_t cursor_pos;
};

struct CommandQueryResult {
    AbiHeader abi;
    const char* id;
    const char* title;
    const char* subtitle;
    const char* value;
    const char* kind;
    uint32_t duration_ms;
    const char* thumbnail_url;
};

struct CommandQueryResponse {
    AbiHeader abi;
    uint8_t status;
    const char* message;
    const CommandQueryResult* results;
    size_t result_count;
};

void (*on_chat_command_query)(const CommandQueryRequest* request,
                              CommandQueryResponse* response);
```

Synchronous callback rules:

- The server calls the callback only for commands owned by the plugin.
- The same `min_role` authorization as command execution applies.
- Returned result pointers MUST remain valid until the callback returns.
- Returned stack-local strings or stack-local result arrays are invalid unless
  the pointed-to storage remains valid until the callback returns.
- The server copies and sanitizes the response immediately after callback
  return.

Example:

```cpp
thread_local std::vector<CommandQueryResult> query_results;

void on_chat_command_query(const CommandQueryRequest* request,
                           CommandQueryResponse* response) {
    if (std::strcmp(request->command_name, "play") != 0 ||
        std::strcmp(request->argument_name, "query") != 0) {
        response->status = static_cast<uint8_t>(CommandQueryStatus::NoResults);
        return;
    }

    query_results = search_tracks(request->query);
    response->status = static_cast<uint8_t>(CommandQueryStatus::Ok);
    response->results = query_results.data();
    response->result_count = query_results.size();
}
```

## Async Query Completion

Slow provider calls return `Pending` from `on_chat_command_query`, then complete
from plugin-owned worker state through the host function.

```cpp
bool (*respond_to_command_query)(void* context,
                                 SessionId session_id,
                                 uint64_t request_id,
                                 const char* command_name,
                                 const char* argument_name,
                                 const CommandQueryResponse* response);
```

Rules:

- The plugin MUST pass the same `session_id`, `request_id`, `command_name`, and
  `argument_name` received in the original `CommandQueryRequest`.
- The async response MUST NOT use status `Pending`.
- The host copies the supplied response before returning from
  `respond_to_command_query`.
- The host function returns `false` when the pending request is expired,
  disconnected, unknown, unowned, or invalid.
- Plugins own their worker lifetime and MUST join or stop workers during plugin
  shutdown.

## Server Requirements

On `CHAT_COMMAND_QUERY`, the server:

1. Requires an authenticated session.
2. Parses and validates the payload.
3. Rejects `command_name` longer than 64 bytes.
4. Rejects `argument_name` longer than 64 bytes.
5. Rejects `query` longer than 512 bytes.
6. Enforces the per-session query rate limit.
7. Resolves `command_name` to a registered plugin command.
8. Applies the command `min_role` authorization check.
9. Requires `argument_name` to identify a `LiveQuery` input.
10. Enforces `min_chars`.
11. Dispatches `on_chat_command_query`.
12. Sends `CHAT_COMMAND_QUERY_RESP` for non-pending responses.
13. Records a pending async request when the callback returns `Pending`.

On `respond_to_command_query`, the server:

1. Verifies that the responding plugin owns `command_name`.
2. Verifies that `argument_name` is a live-query input on that command.
3. Consumes a matching pending request for
   `session_id + request_id + command_name + argument_name`.
4. Drops responses for expired, disconnected, unknown, unowned, or invalid
   pending requests.
5. Copies and sanitizes the plugin response.
6. Sends `CHAT_COMMAND_QUERY_RESP`.

## Limits

| Limit | Value |
| --- | --- |
| Command name | 64 bytes |
| Argument name | 64 bytes |
| Query text | 512 bytes |
| Result count default | 10 |
| Result count hard cap | 25 |
| Result `id` | 512 bytes |
| Result `title` | 512 bytes |
| Result `subtitle` | 512 bytes |
| Result `value` | 512 bytes |
| Result `kind` | 64 bytes |
| Result `thumbnail_url` | 2048 bytes |
| Response `message` | 512 bytes |
| Encoded response payload | 64 KiB |
| Per-session query rate | 5 request burst, 2 requests/sec refill |
| Pending async TTL | 10 seconds |

When plugin `max_results == 0`, the server uses the default result cap. The hard
cap always applies.

## Client Responsibilities

The client owns interactive input state.

- It uses `CHAT_COMMAND_LIST` to identify slash commands.
- It uses `CHAT_COMMAND_INPUT_LIST` to identify live-query arguments.
- It sends `CHAT_COMMAND_QUERY` only for active `LiveQuery` arguments.
- It debounces requests using `debounce_ms`.
- It increments or otherwise changes `request_id` for each query update.
- It stores the latest request id for the active input.
- It ignores stale responses whose `request_id` does not match the active input.
- It treats `TooShort`, `NoResults`, `RateLimited`, and `PluginError` as
  non-fatal input states.
- It inserts selected result `value` into the command argument.
- It sends final execution through existing `CHAT_SEND`.

## Final Command Execution

Live-query selection does not execute a command. It only changes the text or
argument value that will be sent later.

Example:

```text
User selects result:
  value = "https://youtube.example/watch?v=abc"

Client input becomes:
  /play https://youtube.example/watch?v=abc

User presses Enter.

Client sends CHAT_SEND:
  channel_id = 10
  text = "/play https://youtube.example/watch?v=abc"
```

The existing command path invokes `on_chat_command`.

## Provider Values

`value` can be:

- a URL
- a plain search string
- an opaque plugin token
- any string the owning plugin can resolve during `on_chat_command`

If `value` is an opaque token, the plugin is responsible for token expiry and
clean failure when the final command is executed after the token expires.

## Compatibility

- Plugin API v1.0 plugins continue to load.
- Plugin API v1.0 plugins expose no live-query inputs.
- Protocol v1.0 clients continue to receive `CHAT_COMMAND_LIST`.
- Protocol v1.0 clients do not receive `CHAT_COMMAND_INPUT_LIST`.
- Without client live-query support, final slash command execution is unchanged.

