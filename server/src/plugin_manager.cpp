#include <server/plugin_manager.h>

#include <parties/log.h>

#include <toml.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

#include <openssl/sha.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace parties::plugin {
struct Bot {};
} // namespace parties::plugin

namespace parties::server {

struct PluginManager::NativeLibrary {
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif

    ~NativeLibrary() { close(); }

    bool open(const std::filesystem::path& path) {
#ifdef _WIN32
        handle = LoadLibraryW(path.wstring().c_str());
#else
        handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        return handle != nullptr;
    }

    void close() {
        if (!handle) return;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        handle = nullptr;
    }

    void* symbol(const char* name) const {
        if (!handle) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
        return dlsym(handle, name);
#endif
    }
};

struct PluginManager::Bot : plugin::Bot {
    Plugin* owner = nullptr;
    plugin::UserId user_id = 0;
    plugin::ChannelId voice_channel_id = 0;
    std::string display_name;
    bool destroyed = false;
    bool delete_user_on_init_failure = false;
};

struct PluginManager::PluginGrant {
    bool enabled = true;
    std::unordered_set<std::string> permissions;
};

struct PluginManager::Plugin {
    PluginManager* manager = nullptr;
    std::string id;
    std::string name;
    std::string version;
    std::filesystem::path manifest_path;
    std::filesystem::path library_path;
    std::unordered_set<std::string> permissions;
    std::vector<std::pair<std::string, std::string>> variables;
    std::vector<plugin::PluginVariable> host_variables;
    NativeLibrary library;
    plugin::Registration registration{};
    plugin::ShutdownFn shutdown = nullptr;
    std::vector<std::unique_ptr<Bot>> bots;
    std::unordered_set<Bot*> bot_handles;
    std::string last_error;
    bool disabled = false;
};

namespace {

constexpr size_t MAX_LIVE_BOTS_PER_PLUGIN = 64;
constexpr auto PLUGIN_CALLBACK_WARN_AFTER = std::chrono::milliseconds(250);

const std::unordered_set<std::string>& known_plugin_permissions() {
    static const std::unordered_set<std::string> permissions = {
        "read_sessions",
        "read_users",
        "read_channels",
        "read_chat",
        "moderate_chat",
        "create_chat_commands",
        "create_bot_users",
        "send_bot_chat",
        "join_bot_voice",
        "send_bot_audio",
    };
    return permissions;
}

template <typename T>
bool valid_abi_header(const plugin::AbiHeader& abi) {
    return abi.api_major == plugin::API_VERSION_MAJOR &&
           abi.size >= sizeof(plugin::AbiHeader);
}

template <typename T>
bool copy_abi_out(T* out, const T& value) {
    if (!out)
        return false;
    const auto requested = out->abi;
    if (!valid_abi_header<T>(requested))
        return false;
    std::memcpy(out, &value, std::min<size_t>(requested.size, sizeof(T)));
    return true;
}

bool command_def_has_min_role(const plugin::CommandDefinition& command) {
    return command.abi.size >=
        offsetof(plugin::CommandDefinition, min_role) + sizeof(command.min_role);
}

constexpr size_t MIN_COMMAND_DEFINITION_SIZE =
    offsetof(plugin::CommandDefinition, usage) + sizeof(const char*);

const plugin::CommandDefinition* command_at(const plugin::CommandDefinition* commands,
                                            size_t index,
                                            size_t stride) {
    auto* base = reinterpret_cast<const unsigned char*>(commands);
    return reinterpret_cast<const plugin::CommandDefinition*>(base + index * stride);
}

std::string normalize_sha256(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == ':' || c == '-' || std::isspace(c);
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::string> file_sha256_hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    std::array<char, 8192> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = in.gcount();
        if (read > 0) {
            SHA256_Update(&ctx, reinterpret_cast<const unsigned char*>(buffer.data()),
                          static_cast<size_t>(read));
        }
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);

    std::ostringstream out;
    out << std::hex;
    for (unsigned char byte : digest) {
        out.width(2);
        out.fill('0');
        out << static_cast<int>(byte);
    }
    return out.str();
}

} // namespace

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    shutdown();
}

void PluginManager::set_host_services(HostServices services) {
    services_ = std::move(services);
}

std::vector<PluginManager::ChatCommand> PluginManager::chat_commands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chat_commands_;
}

void PluginManager::disable_plugin(Plugin& plugin, std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (plugin.disabled)
        return;
    plugin.disabled = true;
    plugin.last_error = std::string(reason);
    for (auto& bot : plugin.bots) {
        bot->destroyed = true;
        bot->voice_channel_id = 0;
    }
    reap_destroyed_bots(plugin);
    chat_commands_.erase(std::remove_if(chat_commands_.begin(), chat_commands_.end(),
        [&](const ChatCommand& command) { return command.plugin_id == plugin.id; }),
        chat_commands_.end());
    LOG_ERROR("Plugin '{}' disabled: {}", plugin.id, reason);
}

void PluginManager::cleanup_plugin_bots(Plugin& plugin, bool delete_users) {
    struct BotCleanup {
        plugin::UserId user_id = 0;
        plugin::ChannelId voice_channel_id = 0;
        std::string display_name;
        bool delete_user = false;
    };

    std::vector<BotCleanup> bots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bots.reserve(plugin.bots.size());
        for (const auto& bot : plugin.bots) {
            if (!bot->destroyed) {
                bots.push_back(BotCleanup{
                    bot->user_id,
                    bot->voice_channel_id,
                    bot->display_name,
                    bot->delete_user_on_init_failure,
                });
            }
            bot->destroyed = true;
            bot->voice_channel_id = 0;
        }
        reap_destroyed_bots(plugin);
    }

    for (const auto& bot : bots) {
        if (bot.voice_channel_id != 0 && services_.leave_bot_voice)
            services_.leave_bot_voice(bot.user_id, bot.display_name, bot.voice_channel_id);
        if (delete_users && bot.delete_user && services_.delete_bot_user)
            services_.delete_bot_user(bot.user_id);
    }
}

template <typename Fn>
bool PluginManager::invoke_plugin_callback(Plugin& plugin, const char* callback_name, Fn&& fn) {
    if (plugin.disabled)
        return false;

    const auto started = std::chrono::steady_clock::now();
    try {
        fn();
    } catch (const std::exception& e) {
        disable_plugin(plugin, std::string(callback_name) + " threw exception: " + e.what());
        return false;
    } catch (...) {
        disable_plugin(plugin, std::string(callback_name) + " threw unknown exception");
        return false;
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (elapsed > PLUGIN_CALLBACK_WARN_AFTER) {
        LOG_WARN("Plugin '{}' callback '{}' took {} ms",
                 plugin.id,
                 callback_name,
                 std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }
    return true;
}

static bool valid_command_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool valid_argument_name(std::string_view name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_'))
            return false;
    }
    return true;
}

static std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

static bool parse_arg_type(std::string_view text,
                           plugin::CommandArgType& type,
                           bool& rest) {
    rest = false;
    if (text == "string...") {
        type = plugin::CommandArgType::String;
        rest = true;
        return true;
    }
    if (text == "string") {
        type = plugin::CommandArgType::String;
        return true;
    }
    if (text == "bool") {
        type = plugin::CommandArgType::Bool;
        return true;
    }
    if (text == "int8") {
        type = plugin::CommandArgType::Int8;
        return true;
    }
    if (text == "uint8") {
        type = plugin::CommandArgType::UInt8;
        return true;
    }
    if (text == "int16") {
        type = plugin::CommandArgType::Int16;
        return true;
    }
    if (text == "uint16") {
        type = plugin::CommandArgType::UInt16;
        return true;
    }
    if (text == "int32") {
        type = plugin::CommandArgType::Int32;
        return true;
    }
    if (text == "uint32") {
        type = plugin::CommandArgType::UInt32;
        return true;
    }
    if (text == "int64") {
        type = plugin::CommandArgType::Int64;
        return true;
    }
    if (text == "uint64") {
        type = plugin::CommandArgType::UInt64;
        return true;
    }
    if (text == "float") {
        type = plugin::CommandArgType::Float;
        return true;
    }
    if (text == "double") {
        type = plugin::CommandArgType::Double;
        return true;
    }
    return false;
}

static bool read_schema_token(std::string_view input, size_t& pos, std::string_view& token) {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
    if (pos >= input.size())
        return false;

    size_t start = pos;
    while (pos < input.size() && !std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
    token = input.substr(start, pos - start);
    return true;
}

static std::optional<std::vector<PluginManager::ChatCommand::Argument>> parse_command_usage_schema(
    std::string_view command_name,
    std::string_view usage,
    std::string& error) {
    std::vector<PluginManager::ChatCommand::Argument> args;

    size_t pos = 0;
    std::string_view token;
    if (!read_schema_token(usage, pos, token)) {
        error = "usage is empty";
        return std::nullopt;
    }

    std::string expected = "/";
    expected += command_name;
    if (token != expected) {
        error = "usage must start with " + expected;
        return std::nullopt;
    }

    bool saw_optional = false;
    while (read_schema_token(usage, pos, token)) {
        bool required = false;
        if (token.size() >= 3 && token.front() == '{' && token.back() == '}') {
            required = true;
        } else if (token.size() >= 3 && token.front() == '[' && token.back() == ']') {
            required = false;
            saw_optional = true;
        } else {
            error = "usage token '" + std::string(token) + "' is not a typed placeholder";
            return std::nullopt;
        }

        if (required && saw_optional) {
            error = "required arguments cannot follow optional arguments";
            return std::nullopt;
        }

        std::string_view inner = token.substr(1, token.size() - 2);
        size_t colon = inner.find(':');
        if (colon == std::string_view::npos) {
            error = "argument '" + std::string(inner) + "' is missing a type";
            return std::nullopt;
        }

        std::string_view name = inner.substr(0, colon);
        std::string_view type_name = inner.substr(colon + 1);
        if (!valid_argument_name(name)) {
            error = "invalid argument name '" + std::string(name) + "'";
            return std::nullopt;
        }

        plugin::CommandArgType type{};
        bool rest = false;
        if (!parse_arg_type(type_name, type, rest)) {
            error = "unsupported argument type '" + std::string(type_name) + "'";
            return std::nullopt;
        }
        if (rest && pos < usage.size() && !trim_ascii(usage.substr(pos)).empty()) {
            error = "string... argument must be last";
            return std::nullopt;
        }

        auto duplicate = std::find_if(args.begin(), args.end(), [&](const auto& arg) {
            return arg.name == name;
        });
        if (duplicate != args.end()) {
            error = "duplicate argument name '" + std::string(name) + "'";
            return std::nullopt;
        }

        args.push_back(PluginManager::ChatCommand::Argument{
            std::string(name),
            type,
            required,
            rest,
        });
    }

    return args;
}

static bool read_invocation_token(std::string_view input,
                                  size_t& pos,
                                  std::string& token,
                                  std::string& error) {
    token.clear();
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
    if (pos >= input.size())
        return false;

    char quote = 0;
    if (input[pos] == '"' || input[pos] == '\'') {
        quote = input[pos++];
    }

    while (pos < input.size()) {
        char c = input[pos++];
        if (quote) {
            if (c == quote)
                return true;
            if (c == '\\' && pos < input.size()) {
                token.push_back(input[pos++]);
                continue;
            }
            token.push_back(c);
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)))
            return true;
        token.push_back(c);
    }

    if (quote) {
        error = "unterminated quoted string";
        return false;
    }
    return true;
}

static bool parse_signed_integer(const std::string& token,
                                 int64_t min_value,
                                 int64_t max_value,
                                 int64_t& out) {
    if (token.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    long long value = std::strtoll(token.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return false;
    if (value < min_value || value > max_value)
        return false;
    out = static_cast<int64_t>(value);
    return true;
}

static bool parse_unsigned_integer(const std::string& token,
                                   uint64_t max_value,
                                   uint64_t& out) {
    if (token.empty() || token[0] == '-')
        return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long value = std::strtoull(token.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return false;
    if (value > max_value)
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

static bool parse_bool_value(const std::string& token, uint8_t& out) {
    std::string lower;
    lower.reserve(token.size());
    for (char c : token)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "true" || lower == "1") {
        out = 1;
        return true;
    }
    if (lower == "false" || lower == "0") {
        out = 0;
        return true;
    }
    return false;
}

static bool parse_float_value(const std::string& token, double& out) {
    if (token.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    double value = std::strtod(token.c_str(), &end);
    if (errno == ERANGE || !end || *end != '\0')
        return false;
    out = value;
    return true;
}

static const char* command_arg_type_name(plugin::CommandArgType type) {
    switch (type) {
    case plugin::CommandArgType::String: return "string";
    case plugin::CommandArgType::Bool: return "bool";
    case plugin::CommandArgType::Int8: return "int8";
    case plugin::CommandArgType::UInt8: return "uint8";
    case plugin::CommandArgType::Int16: return "int16";
    case plugin::CommandArgType::UInt16: return "uint16";
    case plugin::CommandArgType::Int32: return "int32";
    case plugin::CommandArgType::UInt32: return "uint32";
    case plugin::CommandArgType::Int64: return "int64";
    case plugin::CommandArgType::UInt64: return "uint64";
    case plugin::CommandArgType::Float: return "float";
    case plugin::CommandArgType::Double: return "double";
    }
    return "unknown";
}

static bool fill_argument_value(const PluginManager::ChatCommand::Argument& arg,
                                const std::string& token,
                                plugin::CommandArgumentValue& value,
                                std::string& error) {
    value.abi = plugin::make_abi_header<plugin::CommandArgumentValue>();
    value.name = arg.name.c_str();
    value.type = static_cast<uint8_t>(arg.type);
    value.present = 1;

    switch (arg.type) {
    case plugin::CommandArgType::String:
        return true;
    case plugin::CommandArgType::Bool:
        if (parse_bool_value(token, value.bool_value))
            return true;
        break;
    case plugin::CommandArgType::Int8:
        if (parse_signed_integer(token,
                                 (std::numeric_limits<int8_t>::min)(),
                                 (std::numeric_limits<int8_t>::max)(),
                                 value.i64_value))
            return true;
        break;
    case plugin::CommandArgType::UInt8:
        if (parse_unsigned_integer(token, (std::numeric_limits<uint8_t>::max)(), value.u64_value))
            return true;
        break;
    case plugin::CommandArgType::Int16:
        if (parse_signed_integer(token,
                                 (std::numeric_limits<int16_t>::min)(),
                                 (std::numeric_limits<int16_t>::max)(),
                                 value.i64_value))
            return true;
        break;
    case plugin::CommandArgType::UInt16:
        if (parse_unsigned_integer(token, (std::numeric_limits<uint16_t>::max)(), value.u64_value))
            return true;
        break;
    case plugin::CommandArgType::Int32:
        if (parse_signed_integer(token,
                                 (std::numeric_limits<int32_t>::min)(),
                                 (std::numeric_limits<int32_t>::max)(),
                                 value.i64_value))
            return true;
        break;
    case plugin::CommandArgType::UInt32:
        if (parse_unsigned_integer(token, (std::numeric_limits<uint32_t>::max)(), value.u64_value))
            return true;
        break;
    case plugin::CommandArgType::Int64:
        if (parse_signed_integer(token,
                                 (std::numeric_limits<int64_t>::min)(),
                                 (std::numeric_limits<int64_t>::max)(),
                                 value.i64_value))
            return true;
        break;
    case plugin::CommandArgType::UInt64:
        if (parse_unsigned_integer(token, (std::numeric_limits<uint64_t>::max)(), value.u64_value))
            return true;
        break;
    case plugin::CommandArgType::Float:
    case plugin::CommandArgType::Double:
        if (parse_float_value(token, value.f64_value))
            return true;
        break;
    }

    error = "Invalid argument " + arg.name + ": expected " + command_arg_type_name(arg.type);
    return false;
}

static bool parse_invocation_arguments(
    const std::vector<PluginManager::ChatCommand::Argument>& schema,
    std::string_view input,
    std::vector<plugin::CommandArgumentValue>& values,
    std::vector<std::string>& string_storage,
    std::string& error) {
    values.clear();
    string_storage.clear();
    values.reserve(schema.size());
    string_storage.reserve(schema.size());

    size_t pos = 0;
    for (const auto& arg : schema) {
        plugin::CommandArgumentValue value{};
        value.abi = plugin::make_abi_header<plugin::CommandArgumentValue>();
        value.name = arg.name.c_str();
        value.type = static_cast<uint8_t>(arg.type);
        value.present = 0;

        if (arg.rest) {
            std::string_view rest = trim_ascii(input.substr(pos));
            if (rest.empty()) {
                if (arg.required) {
                    error = "Missing required argument " + arg.name;
                    return false;
                }
                values.push_back(value);
                continue;
            }

            string_storage.emplace_back(rest);
            value.present = 1;
            value.string_value = string_storage.back().c_str();
            values.push_back(value);
            pos = input.size();
            continue;
        }

        std::string token;
        std::string token_error;
        if (!read_invocation_token(input, pos, token, token_error)) {
            if (!token_error.empty()) {
                error = token_error;
                return false;
            }
            if (arg.required) {
                error = "Missing required argument " + arg.name;
                return false;
            }
            values.push_back(value);
            continue;
        }

        if (arg.type == plugin::CommandArgType::String) {
            string_storage.emplace_back(std::move(token));
            value.present = 1;
            value.string_value = string_storage.back().c_str();
            values.push_back(value);
            continue;
        }

        if (!fill_argument_value(arg, token, value, error))
            return false;
        values.push_back(value);
    }

    if (!trim_ascii(input.substr(pos)).empty()) {
        error = "Unexpected extra argument";
        return false;
    }

    return true;
}

static std::vector<std::string> read_string_array(const toml::value& value,
                                                  const char* key) {
    std::vector<std::string> result;
    if (!value.contains(key)) return result;

    try {
        result = toml::find<std::vector<std::string>>(value, key);
    } catch (const std::exception& e) {
        LOG_WARN("Plugin manifest field '{}' is not a string array: {}", key, e.what());
    }
    return result;
}

static std::optional<std::vector<std::pair<std::string, std::string>>> read_plugin_variables(
    const toml::value& manifest,
    std::string& error) {
    std::vector<std::pair<std::string, std::string>> result;
    if (!manifest.contains("variables"))
        return result;

    try {
        auto variables = toml::find<std::map<std::string, std::string>>(manifest, "variables");
        result.reserve(variables.size());
        for (auto& [key, value] : variables) {
            if (key.empty()) {
                error = "variables table contains an empty key";
                return std::nullopt;
            }
            if (value.rfind("env:", 0) == 0) {
                std::string env_name = value.substr(4);
                if (env_name.empty()) {
                    error = "variable '" + key + "' uses an empty environment variable name";
                    return std::nullopt;
                }
                const char* env_value = std::getenv(env_name.c_str());
                if (!env_value) {
                    error = "variable '" + key + "' references missing environment variable '" +
                        env_name + "'";
                    return std::nullopt;
                }
                value = env_value;
            }
            result.emplace_back(std::move(key), std::move(value));
        }
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }

    return result;
}

bool PluginManager::load(const PluginConfig& cfg) {
    shutdown();
    enabled_ = cfg.enabled;
    if (!enabled_) {
        LOG_INFO("Plugin loading disabled");
        return true;
    }

    grants_.clear();
    for (const auto& allow : cfg.allow) {
        PluginGrant grant;
        grant.enabled = allow.enabled;
        for (const auto& permission : allow.permissions) {
            if (known_plugin_permissions().find(permission) == known_plugin_permissions().end()) {
                LOG_WARN("Plugin allow entry '{}' contains unknown permission '{}'",
                         allow.id, permission);
                continue;
            }
            grant.permissions.insert(permission);
        }
        grants_[allow.id] = std::move(grant);
    }
    if (grants_.empty())
        LOG_WARN("Plugin loading enabled, but no plugins are allowed in config");

    std::filesystem::path dir = cfg.directory;
    if (!std::filesystem::exists(dir)) {
        LOG_WARN("Plugin directory '{}' does not exist", dir.string());
        return true;
    }

    size_t loaded = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() != "plugin.toml") continue;
        if (load_manifest(entry.path()))
            ++loaded;
    }

    const size_t command_count = chat_commands().size();
    LOG_INFO("Loaded {} plugin(s), {} chat command(s)", loaded, command_count);
    return true;
}

void PluginManager::shutdown() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        Plugin& plugin = **it;
        if (plugin.registration.on_server_stopping) {
            invoke_plugin_callback(plugin, "on_server_stopping", [&]() {
                plugin.registration.on_server_stopping();
            });
        }
        if (plugin.shutdown) {
            try {
                plugin.shutdown();
            } catch (const std::exception& e) {
                LOG_ERROR("Plugin '{}' shutdown threw exception: {}", plugin.id, e.what());
            } catch (...) {
                LOG_ERROR("Plugin '{}' shutdown threw unknown exception", plugin.id);
            }
        }
        cleanup_plugin_bots(plugin, false);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugins_.clear();
        chat_commands_.clear();
        grants_.clear();
        enabled_ = false;
    }
}

bool PluginManager::load_manifest(const std::filesystem::path& manifest_path) {
    auto parsed = toml::try_parse(manifest_path.string());
    if (parsed.is_err()) {
        LOG_WARN("Failed to parse plugin manifest '{}'", manifest_path.string());
        return false;
    }
    auto manifest = std::move(parsed.unwrap());

    auto plugin = std::make_unique<Plugin>();
    plugin->manager = this;
    plugin->manifest_path = manifest_path;
    plugin->id = toml::find_or(manifest, "id", std::string{});
    plugin->name = toml::find_or(manifest, "name", plugin->id);
    plugin->version = toml::find_or(manifest, "version", std::string{});
    std::string api_version = toml::find_or(manifest, "api_version", std::string{});
    std::string library = toml::find_or(manifest, "library", std::string{});
    std::string expected_sha256 = toml::find_or(manifest, "sha256", std::string{});

    if (plugin->id.empty() || library.empty()) {
        LOG_WARN("Plugin manifest '{}' is missing id or library", manifest_path.string());
        return false;
    }
    if (api_version != "1.0") {
        LOG_WARN("Plugin '{}' has unsupported api_version '{}'", plugin->id, api_version);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto duplicate = std::find_if(plugins_.begin(), plugins_.end(),
            [&](const auto& loaded) { return loaded->id == plugin->id; });
        if (duplicate != plugins_.end()) {
            LOG_WARN("Plugin '{}' from '{}' rejected: duplicate plugin id already loaded from '{}'",
                     plugin->id,
                     manifest_path.string(),
                     (*duplicate)->manifest_path.string());
            return false;
        }
    }

    auto grant_it = grants_.find(plugin->id);
    if (grant_it == grants_.end()) {
        LOG_WARN("Plugin '{}' is not allowed by server config", plugin->id);
        return false;
    }
    if (!grant_it->second.enabled) {
        LOG_INFO("Plugin '{}' is disabled by server config", plugin->id);
        return false;
    }

    auto requested_permissions = read_string_array(manifest, "permissions");
    for (auto& perm : requested_permissions) {
        if (known_plugin_permissions().find(perm) == known_plugin_permissions().end()) {
            LOG_WARN("Plugin '{}' requested unknown permission '{}'", plugin->id, perm);
            continue;
        }
        if (grant_it->second.permissions.find(perm) != grant_it->second.permissions.end()) {
            plugin->permissions.insert(std::move(perm));
        } else {
            LOG_WARN("Plugin '{}' requested ungranted permission '{}'", plugin->id, perm);
        }
    }

    std::string variables_error;
    auto variables = read_plugin_variables(manifest, variables_error);
    if (!variables) {
        LOG_WARN("Plugin '{}' has invalid variables table: {}", plugin->id, variables_error);
        return false;
    }
    plugin->variables = std::move(*variables);
    plugin->host_variables.reserve(plugin->variables.size());
    for (const auto& [key, value] : plugin->variables) {
        plugin->host_variables.push_back(plugin::PluginVariable{
            plugin::make_abi_header<plugin::PluginVariable>(),
            key.c_str(),
            value.c_str(),
        });
    }

    std::filesystem::path library_name(library);
    if (library_name.is_absolute() ||
        library_name.has_root_name() ||
        library_name.has_root_directory() ||
        library_name.has_parent_path() ||
        library_name.filename() != library_name ||
        library_name.filename() == "." ||
        library_name.filename() == "..") {
        LOG_WARN("Plugin '{}' has invalid library path '{}'; expected a bare filename next to plugin.toml",
                 plugin->id, library);
        return false;
    }

    plugin->library_path = manifest_path.parent_path() / library_name.filename();
    if (!expected_sha256.empty()) {
        auto actual_sha256 = file_sha256_hex(plugin->library_path);
        if (!actual_sha256) {
            LOG_WARN("Plugin '{}' could not hash library '{}'", plugin->id, plugin->library_path.string());
            return false;
        }
        if (normalize_sha256(expected_sha256) != *actual_sha256) {
            LOG_WARN("Plugin '{}' library hash mismatch for '{}'", plugin->id, plugin->library_path.string());
            return false;
        }
    }

    if (!plugin->library.open(plugin->library_path)) {
        LOG_WARN("Failed to load plugin '{}' from '{}'", plugin->id, plugin->library_path.string());
        return false;
    }

    auto init = reinterpret_cast<plugin::InitFn>(plugin->library.symbol("parties_plugin_init"));
    plugin->shutdown = reinterpret_cast<plugin::ShutdownFn>(plugin->library.symbol("parties_plugin_shutdown"));
    if (!init) {
        LOG_WARN("Plugin '{}' does not export parties_plugin_init", plugin->id);
        return false;
    }

    plugin::Host host{};
    host.abi = plugin::make_abi_header<plugin::Host>();
    host.context = plugin.get();
    host.log = &PluginManager::host_log;
    host.now_ms = &PluginManager::host_now_ms;
    host.create_chat_commands = &PluginManager::host_create_chat_commands;
    host.create_bot_user = &PluginManager::host_create_bot_user;
    host.destroy_bot_user = &PluginManager::host_destroy_bot_user;
    host.set_bot_display_name = &PluginManager::host_set_bot_display_name;
    host.send_bot_chat = &PluginManager::host_send_bot_chat;
    host.join_bot_voice = &PluginManager::host_join_bot_voice;
    host.leave_bot_voice = &PluginManager::host_leave_bot_voice;
    host.send_bot_voice_packet = &PluginManager::host_send_bot_voice_packet;
    host.user_voice_channel = &PluginManager::host_user_voice_channel;
    host.get_session_info = &PluginManager::host_get_session_info;
    host.get_user_info = &PluginManager::host_get_user_info;
    host.find_user_by_name = &PluginManager::host_find_user_by_name;
    host.get_voice_channel_info = &PluginManager::host_get_voice_channel_info;
    host.get_text_channel_info = &PluginManager::host_get_text_channel_info;
    host.list_voice_channels = &PluginManager::host_list_voice_channels;
    host.list_text_channels = &PluginManager::host_list_text_channels;
    host.bot_voice_channel = &PluginManager::host_bot_voice_channel;
    host.move_bot_to_user_voice = &PluginManager::host_move_bot_to_user_voice;
    host.variables = plugin->host_variables.empty() ? nullptr : plugin->host_variables.data();
    host.variable_count = plugin->host_variables.size();

    plugin->registration = {};
    plugin->registration.abi = plugin::make_abi_header<plugin::Registration>();

    size_t commands_before_init = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_before_init = chat_commands_.size();
    }
    bool init_ok = false;
    try {
        init_ok = init(&host, &plugin->registration);
    } catch (const std::exception& e) {
        LOG_WARN("Plugin '{}' init threw exception: {} (manifest='{}', library='{}')",
                 plugin->id,
                 e.what(),
                 plugin->manifest_path.string(),
                 plugin->library_path.string());
        init_ok = false;
    } catch (...) {
        LOG_WARN("Plugin '{}' init threw unknown exception (manifest='{}', library='{}')",
                 plugin->id,
                 plugin->manifest_path.string(),
                 plugin->library_path.string());
        init_ok = false;
    }

    if (!init_ok) {
        std::string last_error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chat_commands_.resize(commands_before_init);
            last_error = plugin->last_error;
        }
        if (!last_error.empty()) {
            LOG_WARN("Plugin '{}' init failed: {} (manifest='{}', library='{}')",
                     plugin->id,
                     last_error,
                     plugin->manifest_path.string(),
                     plugin->library_path.string());
        } else {
            LOG_WARN("Plugin '{}' init failed: parties_plugin_init returned false "
                     "without reporting a host-side error (manifest='{}', library='{}')",
                     plugin->id,
                     plugin->manifest_path.string(),
                     plugin->library_path.string());
        }
        cleanup_plugin_bots(*plugin, true);
        return false;
    }

    if (plugin->registration.abi.api_major != plugin::API_VERSION_MAJOR ||
        plugin->registration.abi.size != sizeof(plugin::Registration)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chat_commands_.resize(commands_before_init);
        }
        LOG_WARN("Plugin '{}' returned incompatible registration ABI "
                 "(size={}, major={}, expected size={}, major={})",
                 plugin->id,
                 plugin->registration.abi.size,
                 plugin->registration.abi.api_major,
                 sizeof(plugin::Registration),
                 plugin::API_VERSION_MAJOR);
        cleanup_plugin_bots(*plugin, true);
        return false;
    }

    LOG_INFO("Loaded plugin '{}' ({})", plugin->id, plugin->name);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugins_.push_back(std::move(plugin));
    }
    return true;
}

void PluginManager::on_server_started() {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_server_started) {
            invoke_plugin_callback(*plugin, "on_server_started", [&]() {
                plugin->registration.on_server_started();
            });
        }
    }
}

void PluginManager::on_server_stopping() {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_server_stopping) {
            invoke_plugin_callback(*plugin, "on_server_stopping", [&]() {
                plugin->registration.on_server_stopping();
            });
        }
    }
}

void PluginManager::on_session_authenticated(plugin::SessionId session) {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_session_authenticated) {
            invoke_plugin_callback(*plugin, "on_session_authenticated", [&]() {
                plugin->registration.on_session_authenticated(session);
            });
        }
    }
}

void PluginManager::on_session_disconnected(plugin::SessionId session,
                                            plugin::UserId user_id,
                                            plugin::ChannelId voice_channel_id) {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_session_disconnected) {
            invoke_plugin_callback(*plugin, "on_session_disconnected", [&]() {
                plugin->registration.on_session_disconnected(session, user_id, voice_channel_id);
            });
        }
    }
}

std::vector<PluginManager::BotVoiceParticipant> PluginManager::bot_voice_participants(
    plugin::ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BotVoiceParticipant> result;
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (bot->destroyed || bot->voice_channel_id == 0)
                continue;
            if (channel_id != 0 && bot->voice_channel_id != channel_id)
                continue;
            result.push_back(BotVoiceParticipant{
                bot->user_id,
                bot->voice_channel_id,
                bot->display_name,
            });
        }
    }
    return result;
}

uint32_t PluginManager::bot_voice_count(plugin::ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->voice_channel_id == channel_id)
                ++count;
        }
    }
    return count;
}

bool PluginManager::bot_in_voice_channel(plugin::UserId user_id, plugin::ChannelId channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->user_id == user_id && bot->voice_channel_id == channel_id)
                return true;
        }
    }
    return false;
}

void PluginManager::clear_bot_voice_channel(plugin::ChannelId channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->voice_channel_id == channel_id)
                bot->voice_channel_id = 0;
        }
    }
}

bool PluginManager::dispatch_chat_command(plugin::SessionId session,
                                          plugin::UserId user_id,
                                          plugin::ChannelId text_channel_id,
                                          uint8_t caller_role,
                                          std::string_view command_name,
                                          std::string_view args,
                                          std::string_view raw_text,
                                          std::string* error_message) {
    if (error_message)
        error_message->clear();

    ChatCommand command_copy;
    Plugin* owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto command = std::find_if(chat_commands_.begin(), chat_commands_.end(),
            [&](const ChatCommand& cmd) { return cmd.name == command_name; });
        if (command == chat_commands_.end())
            return false;

        command_copy = *command;
        for (const auto& plugin : plugins_) {
            if (plugin->id == command_copy.plugin_id) {
                owner = plugin.get();
                break;
            }
        }
    }
    if (!owner)
        return false;
    if (owner->disabled)
        return true;

    if (caller_role > command_copy.min_role) {
        if (error_message)
            *error_message = "Permission denied";
        return true;
    }

    if (!owner->registration.on_chat_command) {
        LOG_WARN("Plugin '{}' owns command '{}' but has no on_chat_command callback",
                 owner->id, command_name);
        return true;
    }

    std::vector<plugin::CommandArgumentValue> parsed_args;
    std::vector<std::string> string_storage;
    std::string parse_error;
    if (!parse_invocation_arguments(command_copy.arguments, args, parsed_args, string_storage, parse_error)) {
        if (error_message)
            *error_message = parse_error;
        return true;
    }

    std::string command_name_copy(command_name);
    std::string args_copy(args);
    std::string raw_text_copy(raw_text);

    plugin::ChatCommandInvocation invocation{};
    invocation.abi = plugin::make_abi_header<plugin::ChatCommandInvocation>();
    invocation.session_id = session;
    invocation.user_id = user_id;
    invocation.text_channel_id = text_channel_id;
    invocation.caller_role = caller_role;
    invocation.command_name = command_name_copy.c_str();
    invocation.args = args_copy.c_str();
    invocation.raw_text = raw_text_copy.c_str();
    invocation.parsed_args = parsed_args.empty() ? nullptr : parsed_args.data();
    invocation.parsed_arg_count = parsed_args.size();

    invoke_plugin_callback(*owner, "on_chat_command", [&]() {
        owner->registration.on_chat_command(&invocation);
    });
    return true;
}

PluginManager::ChatResult PluginManager::process_chat_message(
    plugin::SessionId session,
    plugin::UserId user_id,
    plugin::ChannelId text_channel_id,
    std::string_view author_name,
    std::string& text,
    uint8_t attachment_count) {
    ChatResult result;

    for (auto& owner : plugins_) {
        Plugin& plugin = *owner;
        if (plugin.disabled)
            continue;
        if (!plugin.registration.on_chat_message)
            continue;
        if (!has_permission(plugin, "read_chat") && !has_permission(plugin, "moderate_chat"))
            continue;

        std::string author_name_copy(author_name);
        plugin::ChatMessage message{};
        message.abi = plugin::make_abi_header<plugin::ChatMessage>();
        message.session_id = session;
        message.author_user_id = user_id;
        message.text_channel_id = text_channel_id;
        message.author_name = author_name_copy.c_str();
        message.text = text.c_str();
        message.attachment_count = attachment_count;

        plugin::ChatDecision decision{};
        decision.abi = plugin::make_abi_header<plugin::ChatDecision>();
        decision.code = static_cast<uint8_t>(plugin::ChatDecisionCode::Continue);

        if (!invoke_plugin_callback(plugin, "on_chat_message", [&]() {
                plugin.registration.on_chat_message(&message, &decision);
            })) {
            continue;
        }

        auto code = static_cast<plugin::ChatDecisionCode>(decision.code);
        if (code == plugin::ChatDecisionCode::Continue)
            continue;

        if (!has_permission(plugin, "moderate_chat")) {
            LOG_WARN("Plugin '{}' tried to moderate chat without permission", plugin.id);
            continue;
        }

        if (code == plugin::ChatDecisionCode::Reject) {
            result.code = ChatResultCode::Reject;
            result.error_message = decision.rejection_reason && decision.rejection_reason[0]
                ? decision.rejection_reason
                : "Message rejected";
            return result;
        }

        if (code == plugin::ChatDecisionCode::ReplaceText) {
            if (decision.replacement_text) {
                text = decision.replacement_text;
            }
            continue;
        }

        LOG_WARN("Plugin '{}' returned invalid chat decision {}", plugin.id, decision.code);
    }

    return result;
}

void PluginManager::host_log(void* context, uint8_t level, const char* message) {
    auto* plugin = static_cast<Plugin*>(context);
    const char* id = plugin ? plugin->id.c_str() : "<unknown>";
    const char* msg = message ? message : "";
    switch (static_cast<plugin::LogLevel>(level)) {
    case plugin::LogLevel::Trace:
    case plugin::LogLevel::Debug:
        LOG_DEBUG("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Info:
        LOG_INFO("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Warn:
        LOG_WARN("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Error:
    default:
        LOG_ERROR("[plugin:{}] {}", id, msg);
        break;
    }
}

uint64_t PluginManager::host_now_ms(void*) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool PluginManager::host_create_chat_commands(void* context,
                                              const plugin::CommandDefinition* commands,
                                              size_t command_count) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    return plugin->manager->register_chat_commands(*plugin, commands, command_count);
}

bool PluginManager::host_create_bot_user(void* context,
                                         const char* key,
                                         const char* display_name,
                                         plugin::BotHandle* out_bot,
                                         plugin::UserId* out_user_id) {
    if (out_bot) *out_bot = nullptr;
    if (out_user_id) *out_user_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;
    std::string plugin_id;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "create_bot_users", "create bot users"))
            return false;
        manager->reap_destroyed_bots(*plugin);
        const auto live_count = std::count_if(plugin->bots.begin(), plugin->bots.end(),
            [](const std::unique_ptr<Bot>& bot) { return !bot->destroyed; });
        if (static_cast<size_t>(live_count) >= MAX_LIVE_BOTS_PER_PLUGIN) {
            LOG_WARN("Plugin '{}' tried to exceed the live bot limit ({})",
                     plugin->id, MAX_LIVE_BOTS_PER_PLUGIN);
            return false;
        }
        plugin_id = plugin->id;
    }

    if (!key || !key[0]) {
        LOG_WARN("Plugin '{}' tried to create a bot user with an empty key", plugin_id);
        return false;
    }
    if (!display_name || !display_name[0]) {
        LOG_WARN("Plugin '{}' tried to create bot user '{}' with an empty display name", plugin_id, key);
        return false;
    }
    if (!manager->services_.create_bot_user) {
        LOG_WARN("Plugin '{}' tried to create a bot user, but bot user creation is unavailable", plugin_id);
        return false;
    }

    auto user = manager->services_.create_bot_user(plugin_id, key, display_name);
    if (!user || user->user_id == 0) {
        LOG_WARN("Plugin '{}' failed to create bot user '{}' ('{}')",
                 plugin_id, key, display_name);
        return false;
    }

    auto bot = std::make_unique<Bot>();
    bot->owner = plugin;
    bot->user_id = user->user_id;
    bot->display_name = display_name;
    bot->delete_user_on_init_failure = user->created;

    plugin::BotHandle handle = static_cast<plugin::BotHandle>(bot.get());
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        plugin->bot_handles.insert(bot.get());
        plugin->bots.push_back(std::move(bot));
    }

    if (out_bot) *out_bot = handle;
    if (out_user_id) *out_user_id = user->user_id;
    LOG_INFO("Plugin '{}' created bot user '{}' (user_id={})",
             plugin_id, display_name, user->user_id);
    return true;
}

bool PluginManager::host_destroy_bot_user(void* context, plugin::BotHandle bot_handle) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;

    plugin::UserId user_id = 0;
    plugin::ChannelId voice_channel_id = 0;
    std::string display_name;
    std::string plugin_id;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "create_bot_users", "destroy bot users"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        user_id = bot->user_id;
        voice_channel_id = bot->voice_channel_id;
        display_name = bot->display_name;
        plugin_id = plugin->id;
    }

    if (voice_channel_id != 0 && manager->services_.leave_bot_voice)
        manager->services_.leave_bot_voice(user_id, display_name, voice_channel_id);

    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        bot->destroyed = true;
        bot->voice_channel_id = 0;
        manager->reap_destroyed_bots(*plugin);
    }
    LOG_INFO("Plugin '{}' destroyed bot user '{}' (user_id={})",
             plugin_id, display_name, user_id);
    return true;
}

bool PluginManager::host_set_bot_display_name(void* context,
                                              plugin::BotHandle bot_handle,
                                              const char* display_name) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;
    if (!display_name || !display_name[0]) {
        LOG_WARN("Plugin '{}' tried to rename a bot user to an empty display name", plugin->id);
        return false;
    }

    plugin::UserId user_id = 0;
    std::string plugin_id;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "create_bot_users", "rename bot users"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        user_id = bot->user_id;
        plugin_id = plugin->id;
    }

    if (!manager->services_.set_bot_display_name) {
        LOG_WARN("Plugin '{}' tried to rename a bot user, but bot user updates are unavailable", plugin->id);
        return false;
    }
    if (!manager->services_.set_bot_display_name(user_id, display_name)) {
        LOG_WARN("Plugin '{}' failed to rename bot user {} to '{}'",
                 plugin_id, user_id, display_name);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        bot->display_name = display_name;
    }
    return true;
}

bool PluginManager::host_send_bot_chat(void* context,
                                       plugin::BotHandle bot_handle,
                                       plugin::ChannelId text_channel_id,
                                       const char* text,
                                       plugin::MessageId* out_message_id) {
    if (out_message_id) *out_message_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;
    if (!text) text = "";

    plugin::UserId user_id = 0;
    std::string display_name;
    std::string plugin_id;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "send_bot_chat", "send bot chat"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        user_id = bot->user_id;
        display_name = bot->display_name;
        plugin_id = plugin->id;
    }

    if (!manager->services_.send_bot_chat) {
        LOG_WARN("Plugin '{}' tried to send bot chat, but bot chat is unavailable", plugin->id);
        return false;
    }

    auto message_id = manager->services_.send_bot_chat(
        user_id, display_name, text_channel_id, text);
    if (!message_id || *message_id == 0) {
        LOG_WARN("Plugin '{}' failed to send bot chat as '{}' (user_id={})",
                 plugin_id, display_name, user_id);
        return false;
    }

    if (out_message_id) *out_message_id = *message_id;
    return true;
}

bool PluginManager::host_join_bot_voice(void* context,
                                        plugin::BotHandle bot_handle,
                                        plugin::ChannelId voice_channel_id) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;

    plugin::UserId user_id = 0;
    plugin::ChannelId old_channel_id = 0;
    std::string display_name;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "join_bot_voice", "join bot voice"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        if (bot->voice_channel_id == voice_channel_id)
            return true;
        user_id = bot->user_id;
        old_channel_id = bot->voice_channel_id;
        display_name = bot->display_name;
    }

    if (!manager->services_.join_bot_voice) {
        LOG_WARN("Plugin '{}' tried to join bot voice, but bot voice is unavailable", plugin->id);
        return false;
    }

    if (old_channel_id != 0) {
        if (manager->services_.leave_bot_voice)
            manager->services_.leave_bot_voice(user_id, display_name, old_channel_id);
    }

    if (!manager->services_.join_bot_voice(user_id, display_name, voice_channel_id))
        return false;

    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        bot->voice_channel_id = voice_channel_id;
    }
    return true;
}

bool PluginManager::host_leave_bot_voice(void* context, plugin::BotHandle bot_handle) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;

    plugin::UserId user_id = 0;
    plugin::ChannelId old_channel_id = 0;
    std::string display_name;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "join_bot_voice", "leave bot voice"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        if (bot->voice_channel_id == 0)
            return true;
        user_id = bot->user_id;
        old_channel_id = bot->voice_channel_id;
        display_name = bot->display_name;
    }

    if (!manager->services_.leave_bot_voice) {
        LOG_WARN("Plugin '{}' tried to leave bot voice, but bot voice is unavailable", plugin->id);
        return false;
    }

    if (!manager->services_.leave_bot_voice(user_id, display_name, old_channel_id))
        return false;

    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        bot->voice_channel_id = 0;
    }
    return true;
}

bool PluginManager::host_send_bot_voice_packet(void* context,
                                               plugin::BotHandle bot_handle,
                                               uint16_t sequence,
                                               const uint8_t* opus_payload,
                                               size_t opus_payload_len) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    auto* manager = plugin->manager;

    plugin::UserId user_id = 0;
    plugin::ChannelId voice_channel_id = 0;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "send_bot_audio", "send bot audio"))
            return false;

        Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
        if (!bot) return false;
        if (bot->voice_channel_id == 0 || !opus_payload || opus_payload_len == 0)
            return false;
        user_id = bot->user_id;
        voice_channel_id = bot->voice_channel_id;
    }

    if (!manager->services_.send_bot_voice_packet) {
        LOG_WARN("Plugin '{}' tried to send bot audio, but bot audio is unavailable", plugin->id);
        return false;
    }

    return manager->services_.send_bot_voice_packet(
        user_id, voice_channel_id, sequence, opus_payload, opus_payload_len);
}

bool PluginManager::host_user_voice_channel(void* context,
                                            plugin::UserId user_id,
                                            plugin::ChannelId* out_voice_channel_id) {
    if (out_voice_channel_id)
        *out_voice_channel_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_voice_channel_id || user_id == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_sessions", "read user voice channel"))
            return false;
    }

    if (auto channel = manager->bot_voice_channel(user_id)) {
        *out_voice_channel_id = *channel;
        return true;
    }

    if (!manager->services_.user_voice_channel) {
        LOG_WARN("Plugin '{}' tried to read user voice channel, but session lookup is unavailable",
                 plugin->id);
        return false;
    }

    auto channel = manager->services_.user_voice_channel(user_id);
    if (!channel)
        return false;

    *out_voice_channel_id = *channel;
    return true;
}

static bool copy_channel_list(const std::vector<plugin::ChannelInfo>& channels,
                              plugin::ChannelInfo* out_channels,
                              size_t* inout_count) {
    if (!inout_count)
        return false;

    const size_t capacity = *inout_count;
    *inout_count = channels.size();

    if (!out_channels)
        return true;

    const size_t to_copy = capacity < channels.size() ? capacity : channels.size();
    for (size_t i = 0; i < to_copy; ++i) {
        if (!copy_abi_out(&out_channels[i], channels[i]))
            return false;
    }

    return capacity >= channels.size();
}

bool PluginManager::host_get_session_info(void* context,
                                          plugin::SessionId session,
                                          plugin::SessionInfo* out_info) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_info || session == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_sessions", "read session info"))
            return false;
    }
    if (!manager->services_.get_session_info)
        return false;

    auto info = manager->services_.get_session_info(session);
    if (!info)
        return false;

    return copy_abi_out(out_info, *info);
}

bool PluginManager::host_get_user_info(void* context,
                                       plugin::UserId user_id,
                                       plugin::UserInfo* out_info) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_info || user_id == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_users", "read user info"))
            return false;
    }
    if (!manager->services_.get_user_info)
        return false;

    auto info = manager->services_.get_user_info(user_id);
    if (!info)
        return false;

    return copy_abi_out(out_info, *info);
}

bool PluginManager::host_find_user_by_name(void* context,
                                           const char* display_name,
                                           plugin::UserId* out_user_id) {
    if (out_user_id)
        *out_user_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_user_id || !display_name || !display_name[0])
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_users", "find user by name"))
            return false;
    }
    if (!manager->services_.find_user_by_name)
        return false;

    auto user_id = manager->services_.find_user_by_name(display_name);
    if (!user_id || *user_id == 0)
        return false;

    *out_user_id = *user_id;
    return true;
}

bool PluginManager::host_get_voice_channel_info(void* context,
                                                plugin::ChannelId channel_id,
                                                plugin::ChannelInfo* out_info) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_info || channel_id == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_channels", "read voice channel info"))
            return false;
    }
    if (!manager->services_.get_voice_channel_info)
        return false;

    auto info = manager->services_.get_voice_channel_info(channel_id);
    if (!info)
        return false;

    return copy_abi_out(out_info, *info);
}

bool PluginManager::host_get_text_channel_info(void* context,
                                               plugin::ChannelId channel_id,
                                               plugin::ChannelInfo* out_info) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_info || channel_id == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_channels", "read text channel info"))
            return false;
    }
    if (!manager->services_.get_text_channel_info)
        return false;

    auto info = manager->services_.get_text_channel_info(channel_id);
    if (!info)
        return false;

    return copy_abi_out(out_info, *info);
}

bool PluginManager::host_list_voice_channels(void* context,
                                             plugin::ChannelInfo* out_channels,
                                             size_t* inout_count) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !inout_count)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_channels", "list voice channels"))
            return false;
    }
    if (!manager->services_.list_voice_channels)
        return false;

    return copy_channel_list(manager->services_.list_voice_channels(),
                             out_channels, inout_count);
}

bool PluginManager::host_list_text_channels(void* context,
                                            plugin::ChannelInfo* out_channels,
                                            size_t* inout_count) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !inout_count)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_channels", "list text channels"))
            return false;
    }
    if (!manager->services_.list_text_channels)
        return false;

    return copy_channel_list(manager->services_.list_text_channels(),
                             out_channels, inout_count);
}

bool PluginManager::host_bot_voice_channel(void* context,
                                           plugin::BotHandle bot_handle,
                                           plugin::ChannelId* out_voice_channel_id) {
    if (out_voice_channel_id)
        *out_voice_channel_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || !out_voice_channel_id)
        return false;
    auto* manager = plugin->manager;

    std::lock_guard<std::mutex> lock(manager->mutex_);
    Bot* bot = manager->get_owned_bot(*plugin, bot_handle);
    if (!bot)
        return false;

    *out_voice_channel_id = bot->voice_channel_id;
    return true;
}

bool PluginManager::host_move_bot_to_user_voice(void* context,
                                                plugin::BotHandle bot_handle,
                                                plugin::UserId user_id) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager || user_id == 0)
        return false;
    auto* manager = plugin->manager;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        if (!manager->check_host_permission(*plugin, "read_sessions", "resolve user voice channel"))
            return false;
    }

    plugin::ChannelId voice_channel_id = 0;
    if (auto channel = manager->bot_voice_channel(user_id)) {
        voice_channel_id = *channel;
    } else {
        if (!manager->services_.user_voice_channel)
            return false;
        auto session_channel = manager->services_.user_voice_channel(user_id);
        if (!session_channel)
            return false;
        voice_channel_id = *session_channel;
    }

    if (voice_channel_id == 0)
        return false;

    return host_join_bot_voice(context, bot_handle, voice_channel_id);
}

bool PluginManager::register_chat_commands(Plugin& plugin,
                                           const plugin::CommandDefinition* commands,
                                           size_t command_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!check_host_permission(plugin, "create_chat_commands", "register chat commands"))
        return false;
    if (!commands && command_count != 0) {
        if (plugin.last_error.empty())
            plugin.last_error = "create_chat_commands received null command array";
        return false;
    }
    size_t command_stride = sizeof(plugin::CommandDefinition);
    if (command_count > 0 && commands) {
        command_stride = commands[0].abi.size;
        if (commands[0].abi.api_major != plugin::API_VERSION_MAJOR ||
            command_stride < MIN_COMMAND_DEFINITION_SIZE) {
            command_stride = sizeof(plugin::CommandDefinition);
        }
    }

    bool all_ok = true;
    for (size_t i = 0; i < command_count; ++i) {
        const auto& cmd = *command_at(commands, i, command_stride);
        if (!valid_abi_header<plugin::CommandDefinition>(cmd.abi) ||
            cmd.abi.size != command_stride ||
            cmd.abi.size < MIN_COMMAND_DEFINITION_SIZE ||
            !cmd.name || !cmd.description || !cmd.usage || !valid_command_name(cmd.name)) {
            LOG_WARN("Plugin '{}' provided invalid chat command at index {}", plugin.id, i);
            if (plugin.last_error.empty()) {
                plugin.last_error = "invalid chat command at index " + std::to_string(i);
            }
            all_ok = false;
            continue;
        }
        std::string schema_error;
        auto arguments = parse_command_usage_schema(cmd.name, cmd.usage, schema_error);
        if (!arguments) {
            LOG_WARN("Plugin '{}' provided invalid usage for command '{}': {}",
                     plugin.id, cmd.name, schema_error);
            if (plugin.last_error.empty()) {
                plugin.last_error = "invalid usage for command '" + std::string(cmd.name) +
                    "': " + schema_error;
            }
            all_ok = false;
            continue;
        }
        auto duplicate = std::find_if(chat_commands_.begin(), chat_commands_.end(),
            [&](const ChatCommand& existing) { return existing.name == cmd.name; });
        if (duplicate != chat_commands_.end()) {
            LOG_WARN("Plugin '{}' tried to register duplicate chat command '{}'", plugin.id, cmd.name);
            if (plugin.last_error.empty()) {
                plugin.last_error = "duplicate chat command '" + std::string(cmd.name) + "'";
            }
            all_ok = false;
            continue;
        }
        chat_commands_.push_back(ChatCommand{
            plugin.id,
            cmd.name,
            cmd.description,
            cmd.usage,
            command_def_has_min_role(cmd) ? cmd.min_role : uint8_t{3},
            std::move(*arguments),
        });
        LOG_INFO("Plugin '{}' registered chat command /{}", plugin.id, cmd.name);
    }

    return all_ok;
}

PluginManager::Bot* PluginManager::get_owned_bot(Plugin& plugin, plugin::BotHandle bot_handle) const {
    if (!bot_handle) {
        LOG_WARN("Plugin '{}' passed a null bot handle", plugin.id);
        return nullptr;
    }

    auto* bot = static_cast<Bot*>(bot_handle);
    if (plugin.bot_handles.find(bot) == plugin.bot_handles.end()) {
        LOG_WARN("Plugin '{}' passed an invalid bot handle", plugin.id);
        return nullptr;
    }
    if (bot->owner != &plugin || bot->destroyed) {
        LOG_WARN("Plugin '{}' passed an invalid bot handle", plugin.id);
        return nullptr;
    }
    return bot;
}

void PluginManager::reap_destroyed_bots(Plugin& plugin) {
    plugin.bots.erase(std::remove_if(plugin.bots.begin(), plugin.bots.end(),
        [&](const std::unique_ptr<Bot>& bot) {
            if (bot->destroyed && bot->voice_channel_id == 0) {
                plugin.bot_handles.erase(bot.get());
                return true;
            }
            return false;
        }),
        plugin.bots.end());
}

std::optional<plugin::ChannelId> PluginManager::bot_voice_channel(plugin::UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->user_id == user_id)
                return bot->voice_channel_id;
        }
    }
    return std::nullopt;
}

bool PluginManager::check_host_permission(Plugin& plugin,
                                          std::string_view permission,
                                          std::string_view action) const {
    if (plugin.disabled) {
        LOG_WARN("Plugin '{}' tried to {} after it was disabled", plugin.id, action);
        return false;
    }
    if (has_permission(plugin, permission))
        return true;

    if (plugin.last_error.empty()) {
        plugin.last_error = "missing permission '" + std::string(permission) +
            "' while trying to " + std::string(action);
    }
    LOG_WARN("Plugin '{}' tried to {} without '{}' permission",
             plugin.id, action, permission);
    return false;
}

bool PluginManager::has_permission(const Plugin& plugin, std::string_view permission) const {
    return plugin.permissions.find(std::string(permission)) != plugin.permissions.end();
}

PluginManager::Plugin* PluginManager::find_command_owner(std::string_view command_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(chat_commands_.begin(), chat_commands_.end(),
        [&](const ChatCommand& cmd) { return cmd.name == command_name; });
    if (it == chat_commands_.end())
        return nullptr;

    for (const auto& plugin : plugins_) {
        if (plugin->id == it->plugin_id)
            return plugin.get();
    }
    return nullptr;
}

} // namespace parties::server
