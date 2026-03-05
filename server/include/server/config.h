#pragma once

#include <string>
#include <cstdint>

namespace parties::server {

struct Config {
    std::string server_name    = "Parties Server";
    std::string listen_ip      = "0.0.0.0";
    uint16_t    port           = 7800;
    int         max_clients    = 64;
    std::string server_password;

    std::string cert_file      = "server.pem";
    std::string key_file       = "server.key.pem";

    std::string db_path        = "parties.db";

    std::string admin_password = "changeme";
    bool        allow_registration = true;

    int         max_users_per_channel = 32;
    int         default_bitrate       = 32000;

    std::string log_level      = "info";

    static Config load(const std::string& toml_path);
};

} // namespace parties::server
