#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace parties::server {

struct ChatConfig {
    int         max_message_length     = 4000;
    int64_t     max_file_size          = 50 * 1024 * 1024;     // 50 MB
    int64_t     max_total_file_storage = 1024LL * 1024 * 1024; // 1 GB
    std::string file_retention         = "time";               // "none", "time", "ring"
    int         file_retention_days    = 30;
    std::string file_storage_path      = "files";
    int         message_retention_days = 0;                    // 0 = keep forever
};

struct Config {
    std::string server_name    = "Parties Server";
    std::string listen_ip      = "0.0.0.0";
    uint16_t    port           = 7800;
    int         max_clients    = 64;
    std::string server_password;

    std::string cert_file      = "server.pem";
    std::string key_file       = "server.key.pem";
    bool        disable_encryption = false;  // Disable QUIC 1-RTT encryption (insecure!)

    std::string db_path        = "parties.db";

    // Identity: fingerprints that get ROOT (Owner) role
    std::vector<std::string> root_fingerprints;

    int         max_users_per_channel = 32;
    int         default_bitrate       = 32000;

    std::string log_level      = "info";

    ChatConfig  chat;

    static Config load(const std::string& toml_path);
};

} // namespace parties::server
