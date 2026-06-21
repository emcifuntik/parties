#include <server/plugin_manager.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace parties::server;

#define TEST_ASSERT(cond, msg) do {                               \
    if (!(cond)) {                                                \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1;                                                 \
    }                                                             \
} while(0)

static bool has_command(const PluginManager& plugins, const char* name) {
    for (const auto& command : plugins.chat_commands()) {
        if (command.name == name)
            return true;
    }
    return false;
}

static size_t command_count(const PluginManager& plugins) {
    return plugins.chat_commands().size();
}

static void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static void replace_text(const fs::path& path,
                         const std::string& needle,
                         const std::string& replacement) {
    std::string text = read_text(path);
    size_t pos = text.find(needle);
    if (pos != std::string::npos)
        text.replace(pos, needle.size(), replacement);
    write_text(path, text);
}

static fs::path prepare_plugin_copy(const fs::path& source,
                                    const fs::path& root,
                                    const std::string& name) {
    fs::path target = root / name;
    fs::remove_all(target);
    fs::create_directories(root);
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return target;
}

int main() {
#ifdef _WIN32
    _putenv_s("PARTIES_BOT_ECHO_PREFIX", "manifest");
#else
    setenv("PARTIES_BOT_ECHO_PREFIX", "manifest", 1);
#endif

    auto tmp = fs::temp_directory_path() / "parties_plugin_policy_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "plugins");

    fs::path source = fs::path(PARTIES_TEST_PLUGIN_DIR) / "bot_echo";
    fs::path target = tmp / "plugins" / "bot_echo";
    if (!fs::exists(source / "plugin.toml")) {
        std::fprintf(stderr, "plugin_policy_test skipped: example plugin not built\n");
        return 0;
    }
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        TEST_ASSERT(plugins.load(cfg), "load with no allow config");
        TEST_ASSERT(plugins.chat_commands().empty(), "default-deny skips plugin");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load explicitly allowed plugin");
        TEST_ASSERT(has_command(plugins, "botping"), "allowed plugin registers botping");
        TEST_ASSERT(has_command(plugins, "botjoin"), "allowed plugin registers botjoin");
        TEST_ASSERT(has_command(plugins, "botapi"), "allowed plugin registers botapi");
        TEST_ASSERT(has_command(plugins, "bottypes"), "allowed plugin registers bottypes");
        TEST_ASSERT(has_command(plugins, "botvars"), "allowed plugin registers botvars");
        TEST_ASSERT(has_command(plugins, "botworker"), "allowed plugin registers botworker");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_bot_users", "send_bot_chat"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin without command permission");
        TEST_ASSERT(plugins.chat_commands().empty(), "missing create_chat_commands blocks init commands");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            false,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load disabled allow entry");
        TEST_ASSERT(plugins.chat_commands().empty(), "disabled allow entry skips plugin");
    }

    {
        fs::path duplicate = tmp / "plugins" / "bot_echo_duplicate";
        fs::copy(source, duplicate, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load duplicate plugin ids");
        TEST_ASSERT(command_count(plugins) == 9, "duplicate plugin id is rejected after first load");
        fs::remove_all(duplicate);
    }

    {
        fs::path root = tmp / "api_version_2";
        fs::path plugin_dir = prepare_plugin_copy(source, root, "bot_echo");
        replace_text(plugin_dir / "plugin.toml", "api_version = \"1.0\"", "api_version = \"2.0\"");

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = root.string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load manifest with api_version 2.0");
        TEST_ASSERT(plugins.chat_commands().empty(), "api_version 2.0 skips plugin");
        fs::remove_all(root);
    }

    {
        fs::path root = tmp / "api_version_missing";
        fs::path plugin_dir = prepare_plugin_copy(source, root, "bot_echo");
        replace_text(plugin_dir / "plugin.toml", "api_version = \"1.0\"\n", "");

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = root.string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load manifest with missing api_version");
        TEST_ASSERT(plugins.chat_commands().empty(), "missing api_version skips plugin");
        fs::remove_all(root);
    }

    {
        fs::path root = tmp / "init_false";
        fs::path plugin_dir = prepare_plugin_copy(source, root, "bot_echo");
        replace_text(plugin_dir / "plugin.toml",
                     "variables = { echo_prefix = \"env:PARTIES_BOT_ECHO_PREFIX\" }",
                     "variables = { echo_prefix = \"env:PARTIES_BOT_ECHO_PREFIX\", mode = \"init_false_after_commands\" }");

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = root.string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin that returns false after commands");
        TEST_ASSERT(plugins.chat_commands().empty(), "init false rolls back commands");
        fs::remove_all(root);
    }

    {
        fs::path root = tmp / "bad_registration_abi";
        fs::path plugin_dir = prepare_plugin_copy(source, root, "bot_echo");
        replace_text(plugin_dir / "plugin.toml",
                     "variables = { echo_prefix = \"env:PARTIES_BOT_ECHO_PREFIX\" }",
                     "variables = { echo_prefix = \"env:PARTIES_BOT_ECHO_PREFIX\", mode = \"bad_registration_abi\" }");

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = root.string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin with bad registration ABI");
        TEST_ASSERT(plugins.chat_commands().empty(), "bad registration ABI rolls back commands");
        fs::remove_all(root);
    }

    {
        PluginManager plugins;
        PluginManager::HostServices services;
        int created_bots = 0;
        int sent_messages = 0;
        services.create_bot_user = [&](std::string_view, std::string_view, std::string_view) {
            ++created_bots;
            return std::optional<PluginManager::HostServices::BotUserResult>(
                PluginManager::HostServices::BotUserResult{42, created_bots == 1});
        };
        services.send_bot_chat = [&](parties::plugin::UserId, std::string_view,
                                     parties::plugin::ChannelId, std::string_view) {
            ++sent_messages;
            return std::optional<parties::plugin::MessageId>(1);
        };
        plugins.set_host_services(std::move(services));

        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands", "create_bot_users"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin without send_bot_chat grant");
        TEST_ASSERT(plugins.dispatch_chat_command(1, 1, 1, 3, "botping", "denied", "/botping denied"),
                    "dispatch botping without send_bot_chat grant");
        TEST_ASSERT(created_bots == 1, "runtime permission test creates bot");
        TEST_ASSERT(sent_messages == 0, "withheld send_bot_chat fails closed at runtime");
    }

    {
        PluginManager plugins;
        PluginManager::HostServices services;
        int sent_messages = 0;
        services.create_bot_user = [&](std::string_view, std::string_view, std::string_view) {
            return std::optional<PluginManager::HostServices::BotUserResult>(
                PluginManager::HostServices::BotUserResult{43, true});
        };
        services.send_bot_chat = [&](parties::plugin::UserId, std::string_view,
                                     parties::plugin::ChannelId, std::string_view) {
            ++sent_messages;
            return std::optional<parties::plugin::MessageId>(1);
        };
        plugins.set_host_services(std::move(services));

        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands", "create_bot_users", "send_bot_cht"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin with typoed permission grant");
        TEST_ASSERT(plugins.dispatch_chat_command(1, 1, 1, 3, "botping", "denied", "/botping denied"),
                    "dispatch botping with typoed send permission");
        TEST_ASSERT(sent_messages == 0, "typoed send_bot_chat grant fails closed");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands", "read_chat"}
        });
        TEST_ASSERT(plugins.load(cfg), "load moderation plugin without moderate_chat");
        std::string text = "bot-moderate-reject";
        auto result = plugins.process_chat_message(1, 1, 1, "tester", text, 0);
        TEST_ASSERT(result.code == PluginManager::ChatResultCode::Continue,
                    "moderation without permission is ignored");
        TEST_ASSERT(text == "bot-moderate-reject", "ignored moderation leaves text unchanged");
    }

    {
        PluginManager plugins;
        PluginManager::HostServices services;
        int leave_calls = 0;
        services.create_bot_user = [&](std::string_view, std::string_view, std::string_view) {
            return std::optional<PluginManager::HostServices::BotUserResult>(
                PluginManager::HostServices::BotUserResult{44, true});
        };
        services.join_bot_voice = [&](parties::plugin::UserId, std::string_view,
                                      parties::plugin::ChannelId) {
            return true;
        };
        services.leave_bot_voice = [&](parties::plugin::UserId, std::string_view,
                                       parties::plugin::ChannelId) {
            ++leave_calls;
            return true;
        };
        plugins.set_host_services(std::move(services));

        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands", "create_bot_users", "join_bot_voice"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin for destroy cleanup test");
        TEST_ASSERT(plugins.dispatch_chat_command(1, 1, 1, 3, "botjoin", "", "/botjoin"),
                    "dispatch botjoin for destroy cleanup test");
        TEST_ASSERT(plugins.bot_voice_count(1) == 1, "bot voice count after join");
        TEST_ASSERT(plugins.dispatch_chat_command(1, 1, 1, 3, "botreset", "", "/botreset"),
                    "dispatch botreset while bot is in voice");
        TEST_ASSERT(leave_calls == 1, "destroyed in-voice bot calls leave service");
        TEST_ASSERT(plugins.bot_voice_count(1) == 0, "destroyed bot removed from voice count");
    }

    {
        PluginManager plugins;
        PluginManager::HostServices services;
        int leave_calls = 0;
        services.create_bot_user = [&](std::string_view, std::string_view, std::string_view) {
            return std::optional<PluginManager::HostServices::BotUserResult>(
                PluginManager::HostServices::BotUserResult{45, true});
        };
        services.join_bot_voice = [&](parties::plugin::UserId, std::string_view,
                                      parties::plugin::ChannelId) {
            return true;
        };
        services.leave_bot_voice = [&](parties::plugin::UserId, std::string_view,
                                       parties::plugin::ChannelId) {
            ++leave_calls;
            return true;
        };
        plugins.set_host_services(std::move(services));

        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands", "create_bot_users", "join_bot_voice"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin for unload cleanup test");
        TEST_ASSERT(plugins.dispatch_chat_command(1, 1, 1, 3, "botjoin", "", "/botjoin"),
                    "dispatch botjoin for unload cleanup test");
        TEST_ASSERT(plugins.bot_voice_count(1) == 1, "bot voice count before unload");
        plugins.shutdown();
        TEST_ASSERT(leave_calls == 1, "unload cleans up in-voice bot");
        TEST_ASSERT(plugins.bot_voice_count(1) == 0, "unload removes bot from voice count");
    }

    {
        fs::path bad = tmp / "plugins" / "bad_library_path";
        fs::create_directories(bad);
        write_text(bad / "plugin.toml",
            "id = \"parties.bad_library_path\"\n"
            "name = \"Bad Library Path\"\n"
            "version = \"0.1.0\"\n"
            "api_version = \"1.0\"\n"
            "library = \"..\\\\evil.dll\"\n"
            "permissions = [\"create_chat_commands\"]\n");

        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (bad.parent_path()).string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.bad_library_path",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load manifest with invalid library path");
        TEST_ASSERT(plugins.chat_commands().empty(), "invalid library path skips plugin");
        fs::remove_all(bad);
    }

    fs::remove_all(tmp);
    std::fprintf(stderr, "plugin_policy_test passed\n");
    return 0;
}
