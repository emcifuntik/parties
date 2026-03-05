#pragma once

#include <parties/types.h>
#include <parties/permissions.h>

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

struct sqlite3;

namespace parties::server {

struct UserRow {
    UserId      id = 0;
    PublicKey    public_key{};
    std::string display_name;
    std::string fingerprint;
    int         role = 3;
    std::string created_at;
    std::string last_login;
};

struct ChannelRow {
    ChannelId   id = 0;
    std::string name;
    int         max_users = 0;    // 0 = unlimited (use server default)
    int         sort_order = 0;
};

class Database {
public:
    Database();
    ~Database();

    // Open/create the database at the given path
    bool open(const std::string& path);
    void close();

    // --- Users ---
    bool create_user(const PublicKey& pubkey, const std::string& display_name,
                     const std::string& fingerprint, Role role = Role::User);
    std::optional<UserRow> get_user_by_pubkey(const PublicKey& pubkey);
    std::optional<UserRow> get_user_by_id(UserId id);
    bool update_last_login(UserId id);
    bool update_display_name(UserId id, const std::string& display_name);
    bool set_user_role(UserId id, Role role);
    std::vector<UserRow> get_all_users();
    bool delete_user(UserId id);

    // --- Channels ---
    bool create_channel(const std::string& name, int max_users = 0, int sort_order = 0);
    std::optional<ChannelRow> get_channel(ChannelId id);
    std::vector<ChannelRow> get_all_channels();
    bool delete_channel(ChannelId id);

    // --- Channel permissions ---
    bool set_channel_permission(ChannelId channel_id, Role role, uint32_t permissions);
    std::optional<uint32_t> get_channel_permission(ChannelId channel_id, Role role);

    // --- Server metadata ---
    bool set_meta(const std::string& key, const std::string& value);
    std::optional<std::string> get_meta(const std::string& key);

    // --- Admin ---
    bool has_any_users();

private:
    bool exec(const std::string& sql);
    bool create_schema();

    sqlite3* db_ = nullptr;
};

} // namespace parties::server
