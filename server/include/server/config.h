#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace parties::server {

struct ChatConfig {
    int         max_message_length     = 4000;
    int64_t     max_file_size          = 50 * 1024 * 1024;
    int64_t     max_total_file_storage = 1024LL * 1024 * 1024;
    std::string file_retention         = "time";
    int         file_retention_days    = 30;
    std::string file_storage_path      = "files";
    int         message_retention_days = 0;
};

struct Config {
    std::string server_name    = "Parties Server";
    std::string listen_ip      = "0.0.0.0";
    uint16_t    port           = 7800;
    int         max_clients    = 64;
    std::string server_password;

    std::string cert_file      = "server.pem";
    std::string key_file       = "server.key.pem";

    std::string db_path        = "parties.db";

    std::vector<std::string> root_fingerprints;

    int         max_users_per_channel = 32;
    int         default_bitrate       = 32000;

    std::string log_level      = "info";

    ChatConfig  chat;

    static Config load(const std::string& toml_path);
};

}
