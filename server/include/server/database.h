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

struct TextChannelRow {
    uint32_t    id = 0;
    std::string name;
    int         sort_order = 0;
};

struct MessageRow {
    uint64_t    id = 0;
    uint32_t    channel_id = 0;
    uint32_t    sender_id = 0;
    std::string sender_name;
    std::string text;
    bool        pinned = false;
    uint64_t    created_at = 0;  // unix timestamp (seconds)
};

struct AttachmentRow {
    uint64_t    id = 0;
    uint64_t    message_id = 0;
    std::string file_name;
    int64_t     file_size = 0;
    std::string mime_type;
    std::string disk_path;
    bool        uploaded = false;
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
    bool rename_channel(ChannelId id, const std::string& new_name);

    // --- Channel permissions ---
    bool set_channel_permission(ChannelId channel_id, Role role, uint32_t permissions);
    std::optional<uint32_t> get_channel_permission(ChannelId channel_id, Role role);

    // --- Server metadata ---
    bool set_meta(const std::string& key, const std::string& value);
    std::optional<std::string> get_meta(const std::string& key);

    // --- Text channels ---
    bool create_text_channel(const std::string& name, int sort_order = 0);
    std::optional<TextChannelRow> get_text_channel(uint32_t id);
    std::vector<TextChannelRow> get_all_text_channels();
    bool delete_text_channel(uint32_t id);

    // --- Messages ---
    uint64_t insert_message(uint32_t channel_id, uint32_t sender_id,
                            const std::string& sender_name, const std::string& text,
                            uint64_t timestamp);  // returns message ID, 0 on error
    std::vector<MessageRow> get_messages(uint32_t channel_id, uint64_t before_id, int limit);
    std::optional<MessageRow> get_message(uint64_t message_id);
    bool soft_delete_message(uint64_t message_id);
    bool pin_message(uint64_t message_id);
    bool unpin_message(uint64_t message_id);
    std::vector<MessageRow> get_pinned_messages(uint32_t channel_id);
    std::vector<MessageRow> search_messages(uint32_t channel_id, const std::string& query,
                                            uint64_t before_id, int limit);

    // --- File attachments ---
    uint64_t insert_attachment(uint64_t message_id, const std::string& file_name,
                               int64_t file_size, const std::string& mime_type,
                               const std::string& disk_path);
    bool mark_attachment_uploaded(uint64_t attachment_id);
    std::optional<AttachmentRow> get_attachment(uint64_t file_id);
    std::vector<AttachmentRow> get_attachments_for_message(uint64_t message_id);

    // --- Retention ---
    int purge_old_messages(int days);
    int purge_old_files(int days, const std::string& storage_path);
    int64_t get_total_file_size();
    int purge_oldest_files(int64_t max_size, const std::string& storage_path);

    // --- Admin ---
    bool has_any_users();

private:
    bool exec(const std::string& sql);
    bool create_schema();

    sqlite3* db_ = nullptr;
};

} // namespace parties::server
