#include <server/config.h>

#include <toml.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace parties::server {

// Helper: get env var or empty string
static std::string env(const char* name) {
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        std::string result(buf);
        free(buf);
        return result;
    }
    return "";
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

Config Config::load(const std::string& toml_path) {
    Config cfg;

    // ── Load TOML file (if present) ──
    std::ifstream test(toml_path);
    if (!test.good()) {
        std::fprintf(stderr, "Warning: %s not found, using defaults.\n", toml_path.c_str());
    } else {
        test.close();

        auto result = toml::try_parse(toml_path);
        if (result.is_err()) {
            std::fprintf(stderr, "Warning: Failed to parse %s, using defaults.\n", toml_path.c_str());
        } else {
            auto data = std::move(result.unwrap());

            if (data.contains("server")) {
                auto& s = data.at("server");
                cfg.server_name    = toml::find_or(s, "name", cfg.server_name);
                cfg.listen_ip      = toml::find_or(s, "listen_ip", cfg.listen_ip);
                cfg.port           = static_cast<uint16_t>(toml::find_or(s, "port", static_cast<int>(cfg.port)));
                cfg.max_clients    = toml::find_or(s, "max_clients", cfg.max_clients);
                cfg.server_password = toml::find_or(s, "password", cfg.server_password);
            }

            if (data.contains("tls")) {
                auto& t = data.at("tls");
                cfg.cert_file = toml::find_or(t, "cert_file", cfg.cert_file);
                cfg.key_file  = toml::find_or(t, "key_file", cfg.key_file);
            }

            if (data.contains("database")) {
                auto& d = data.at("database");
                cfg.db_path = toml::find_or(d, "path", cfg.db_path);
            }

            if (data.contains("identity")) {
                auto& id = data.at("identity");
                if (id.contains("root_fingerprints")) {
                    auto arr = toml::find<std::vector<std::string>>(id, "root_fingerprints");
                    cfg.root_fingerprints = std::move(arr);
                }
            }

            if (data.contains("voice")) {
                auto& v = data.at("voice");
                cfg.max_users_per_channel = toml::find_or(v, "max_users_per_channel", cfg.max_users_per_channel);
                cfg.default_bitrate       = toml::find_or(v, "default_bitrate", cfg.default_bitrate);
            }

            if (data.contains("logging")) {
                auto& l = data.at("logging");
                cfg.log_level = toml::find_or(l, "level", cfg.log_level);
            }
        }
    }

    // ── Environment variable overrides (highest priority) ──
    auto e = env("PARTIES_SERVER_NAME");
    if (!e.empty()) cfg.server_name = e;

    e = env("PARTIES_LISTEN_IP");
    if (!e.empty()) cfg.listen_ip = e;

    e = env("PARTIES_PORT");
    if (!e.empty()) cfg.port = static_cast<uint16_t>(std::stoi(e));

    e = env("PARTIES_MAX_CLIENTS");
    if (!e.empty()) cfg.max_clients = std::stoi(e);

    e = env("PARTIES_PASSWORD");
    if (!e.empty()) cfg.server_password = e;

    e = env("PARTIES_CERT_FILE");
    if (!e.empty()) cfg.cert_file = e;

    e = env("PARTIES_KEY_FILE");
    if (!e.empty()) cfg.key_file = e;

    e = env("PARTIES_DB_PATH");
    if (!e.empty()) cfg.db_path = e;

    // Comma-separated list of Ed25519 fingerprints
    e = env("PARTIES_ROOT_FINGERPRINTS");
    if (!e.empty()) {
        cfg.root_fingerprints.clear();
        std::istringstream ss(e);
        std::string fp;
        while (std::getline(ss, fp, ',')) {
            if (!fp.empty()) cfg.root_fingerprints.push_back(fp);
        }
    }

    e = env("PARTIES_MAX_USERS_PER_CHANNEL");
    if (!e.empty()) cfg.max_users_per_channel = std::stoi(e);

    e = env("PARTIES_DEFAULT_BITRATE");
    if (!e.empty()) cfg.default_bitrate = std::stoi(e);

    e = env("PARTIES_LOG_LEVEL");
    if (!e.empty()) cfg.log_level = e;

    return cfg;
}

} // namespace parties::server
