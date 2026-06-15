#include <server/plugin_manager.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
        TEST_ASSERT(command_count(plugins) == 8, "duplicate plugin id is rejected after first load");
        fs::remove_all(duplicate);
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
