#include <server/config.h>

#include <toml.hpp>
#include <cstdio>
#include <fstream>

namespace parties::server {

Config Config::load(const std::string& toml_path) {
    Config cfg;

    std::ifstream test(toml_path);
    if (!test.good()) {
        std::fprintf(stderr, "Warning: %s not found, using defaults.\n", toml_path.c_str());
        return cfg;
    }
    test.close();

    auto result = toml::try_parse(toml_path);
    if (result.is_err()) {
        std::fprintf(stderr, "Warning: Failed to parse %s, using defaults.\n", toml_path.c_str());
        return cfg;
    }
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

    return cfg;
}

} // namespace parties::server
