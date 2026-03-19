#include <server/database.h>

#include <sqlite3.h>
#include <parties/log.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace parties::server {

Database::Database() = default;

Database::~Database() {
    close();
}

bool Database::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("Failed to open {}: {}", path, sqlite3_errmsg(db_));
        return false;
    }

    // Enable WAL and foreign keys
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");

    if (!create_schema()) {
        LOG_ERROR("Failed to create schema");
        close();
        return false;
    }

    LOG_INFO("Opened database {}", path);
    return true;
}

void Database::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("SQL error: {}", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::create_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            public_key    BLOB NOT NULL UNIQUE,
            display_name  TEXT NOT NULL DEFAULT '',
            fingerprint   TEXT NOT NULL,
            role          INTEGER NOT NULL DEFAULT 3,
            created_at    TEXT NOT NULL DEFAULT (datetime('now')),
            last_login    TEXT
        );

        CREATE TABLE IF NOT EXISTS channels (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            name       TEXT NOT NULL UNIQUE,
            max_users  INTEGER NOT NULL DEFAULT 0,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS channel_permissions (
            channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
            role       INTEGER NOT NULL,
            permission INTEGER NOT NULL,
            PRIMARY KEY (channel_id, role)
        );

        CREATE TABLE IF NOT EXISTS server_meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        INSERT OR IGNORE INTO channels (id, name, sort_order) VALUES (1, 'General', 0);

        -- Text channels (separate from voice channels)
        CREATE TABLE IF NOT EXISTS text_channels (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            name       TEXT NOT NULL UNIQUE,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        INSERT OR IGNORE INTO text_channels (id, name, sort_order) VALUES (1, 'general', 0);

        -- Chat messages
        CREATE TABLE IF NOT EXISTS messages (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            channel_id  INTEGER NOT NULL REFERENCES text_channels(id) ON DELETE CASCADE,
            sender_id   INTEGER NOT NULL REFERENCES users(id),
            sender_name TEXT NOT NULL,
            text        TEXT NOT NULL DEFAULT '',
            pinned      INTEGER NOT NULL DEFAULT 0,
            created_at  INTEGER NOT NULL,
            deleted     INTEGER NOT NULL DEFAULT 0
        );

        CREATE INDEX IF NOT EXISTS idx_messages_channel_id
            ON messages(channel_id, id DESC);
        CREATE INDEX IF NOT EXISTS idx_messages_pinned
            ON messages(channel_id, pinned) WHERE pinned = 1;

        -- File attachments (files stored on disk)
        CREATE TABLE IF NOT EXISTS file_attachments (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            message_id  INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
            file_name   TEXT NOT NULL,
            file_size   INTEGER NOT NULL,
            mime_type   TEXT NOT NULL DEFAULT 'application/octet-stream',
            disk_path   TEXT NOT NULL,
            uploaded    INTEGER NOT NULL DEFAULT 0,
            created_at  TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE INDEX IF NOT EXISTS idx_attachments_message
            ON file_attachments(message_id);

        -- Full-text search on messages
        CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
            text,
            content='messages',
            content_rowid='id'
        );

        -- FTS sync triggers
        CREATE TRIGGER IF NOT EXISTS messages_fts_insert AFTER INSERT ON messages BEGIN
            INSERT INTO messages_fts(rowid, text) VALUES (new.id, new.text);
        END;

        CREATE TRIGGER IF NOT EXISTS messages_fts_delete AFTER UPDATE OF deleted ON messages
            WHEN new.deleted = 1 BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, text)
                VALUES ('delete', old.id, old.text);
        END;
    )SQL";

    return exec(schema);
}

// --- Users ---

bool Database::create_user(const PublicKey& pubkey, const std::string& display_name,
                           const std::string& fingerprint, Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO users (public_key, display_name, fingerprint, role) "
                      "VALUES (?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_blob(stmt, 1, pubkey.data(), static_cast<int>(pubkey.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, static_cast<int>(role));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static UserRow read_user_row(sqlite3_stmt* stmt) {
    UserRow row;
    row.id = static_cast<UserId>(sqlite3_column_int(stmt, 0));

    auto* pk_blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
    int pk_len = sqlite3_column_bytes(stmt, 1);
    if (pk_blob && pk_len == 32)
        std::memcpy(row.public_key.data(), pk_blob, 32);

    row.display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    row.role = sqlite3_column_int(stmt, 4);
    row.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
        row.last_login = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

    return row;
}

std::optional<UserRow> Database::get_user_by_pubkey(const PublicKey& pubkey) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, public_key, display_name, fingerprint, role, created_at, last_login "
                      "FROM users WHERE public_key = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_blob(stmt, 1, pubkey.data(), static_cast<int>(pubkey.size()), SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    auto row = read_user_row(stmt);
    sqlite3_finalize(stmt);
    return row;
}

std::optional<UserRow> Database::get_user_by_id(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, public_key, display_name, fingerprint, role, created_at, last_login "
                      "FROM users WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    auto row = read_user_row(stmt);
    sqlite3_finalize(stmt);
    return row;
}

bool Database::update_last_login(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET last_login = datetime('now') WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::update_display_name(UserId id, const std::string& display_name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET display_name = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::set_user_role(UserId id, Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET role = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(role));
    sqlite3_bind_int(stmt, 2, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<UserRow> Database::get_all_users() {
    std::vector<UserRow> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, public_key, display_name, fingerprint, role, created_at, last_login "
                      "FROM users ORDER BY id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(read_user_row(stmt));

    sqlite3_finalize(stmt);
    return result;
}

bool Database::delete_user(UserId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM users WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Channels ---

bool Database::create_channel(const std::string& name, int max_users, int sort_order) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO channels (name, max_users, sort_order) VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_users);
    sqlite3_bind_int(stmt, 3, sort_order);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<ChannelRow> Database::get_channel(ChannelId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, max_users, sort_order FROM channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    ChannelRow row;
    row.id = static_cast<ChannelId>(sqlite3_column_int(stmt, 0));
    row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.max_users = sqlite3_column_int(stmt, 2);
    row.sort_order = sqlite3_column_int(stmt, 3);

    sqlite3_finalize(stmt);
    return row;
}

std::vector<ChannelRow> Database::get_all_channels() {
    std::vector<ChannelRow> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, max_users, sort_order FROM channels ORDER BY sort_order, id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChannelRow row;
        row.id = static_cast<ChannelId>(sqlite3_column_int(stmt, 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.max_users = sqlite3_column_int(stmt, 2);
        row.sort_order = sqlite3_column_int(stmt, 3);
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::delete_channel(ChannelId id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::rename_channel(ChannelId id, const std::string& new_name) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE channels SET name = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Channel permissions ---

bool Database::set_channel_permission(ChannelId channel_id, Role role, uint32_t permissions) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO channel_permissions (channel_id, role, permission) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    sqlite3_bind_int(stmt, 2, static_cast<int>(role));
    sqlite3_bind_int(stmt, 3, static_cast<int>(permissions));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<uint32_t> Database::get_channel_permission(ChannelId channel_id, Role role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT permission FROM channel_permissions "
                      "WHERE channel_id = ? AND role = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    sqlite3_bind_int(stmt, 2, static_cast<int>(role));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    uint32_t perm = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);
    return perm;
}

// --- Server metadata ---

bool Database::set_meta(const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO server_meta (key, value) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<std::string> Database::get_meta(const std::string& key) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM server_meta WHERE key = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    std::string val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return val;
}

// --- Text channels ---

bool Database::create_text_channel(const std::string& name, int sort_order) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO text_channels (name, sort_order) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, sort_order);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<TextChannelRow> Database::get_text_channel(uint32_t id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, sort_order FROM text_channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    TextChannelRow row;
    row.id = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
    row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.sort_order = sqlite3_column_int(stmt, 2);

    sqlite3_finalize(stmt);
    return row;
}

std::vector<TextChannelRow> Database::get_all_text_channels() {
    std::vector<TextChannelRow> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, sort_order FROM text_channels ORDER BY sort_order, id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TextChannelRow row;
        row.id = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.sort_order = sqlite3_column_int(stmt, 2);
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::delete_text_channel(uint32_t id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM text_channels WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, static_cast<int>(id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Messages ---

uint64_t Database::insert_message(uint32_t channel_id, uint32_t sender_id,
                                   const std::string& sender_name, const std::string& text,
                                   uint64_t timestamp) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO messages (channel_id, sender_id, sender_name, text, created_at) "
                      "VALUES (?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    sqlite3_bind_int(stmt, 2, static_cast<int>(sender_id));
    sqlite3_bind_text(stmt, 3, sender_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(timestamp));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }

    auto id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
    sqlite3_finalize(stmt);
    return id;
}

static MessageRow read_message_row(sqlite3_stmt* stmt) {
    MessageRow row;
    row.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    row.channel_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));
    row.sender_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
    row.sender_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    row.text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    row.pinned = sqlite3_column_int(stmt, 5) != 0;
    row.created_at = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
    return row;
}

std::vector<MessageRow> Database::get_messages(uint32_t channel_id, uint64_t before_id, int limit) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql;

    if (before_id == 0) {
        sql = "SELECT id, channel_id, sender_id, sender_name, text, pinned, created_at "
              "FROM messages WHERE channel_id = ? AND deleted = 0 "
              "ORDER BY id DESC LIMIT ?";
    } else {
        sql = "SELECT id, channel_id, sender_id, sender_name, text, pinned, created_at "
              "FROM messages WHERE channel_id = ? AND deleted = 0 AND id < ? "
              "ORDER BY id DESC LIMIT ?";
    }

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
    if (before_id == 0) {
        sqlite3_bind_int(stmt, 2, limit);
    } else {
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(before_id));
        sqlite3_bind_int(stmt, 3, limit);
    }

    std::vector<MessageRow> result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(read_message_row(stmt));

    sqlite3_finalize(stmt);

    // Results come newest-first from DB; reverse so oldest is first
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<MessageRow> Database::get_message(uint64_t message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, channel_id, sender_id, sender_name, text, pinned, created_at "
                      "FROM messages WHERE id = ? AND deleted = 0";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    auto row = read_message_row(stmt);
    sqlite3_finalize(stmt);
    return row;
}

bool Database::soft_delete_message(uint64_t message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE messages SET deleted = 1 WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::pin_message(uint64_t message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE messages SET pinned = 1 WHERE id = ? AND deleted = 0";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::unpin_message(uint64_t message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE messages SET pinned = 0 WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<MessageRow> Database::get_pinned_messages(uint32_t channel_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, channel_id, sender_id, sender_name, text, pinned, created_at "
                      "FROM messages WHERE channel_id = ? AND pinned = 1 AND deleted = 0 "
                      "ORDER BY id DESC";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));

    std::vector<MessageRow> result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(read_message_row(stmt));

    sqlite3_finalize(stmt);
    return result;
}

std::vector<MessageRow> Database::search_messages(uint32_t channel_id, const std::string& query,
                                                   uint64_t before_id, int limit) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql;

    if (before_id == 0) {
        sql = "SELECT m.id, m.channel_id, m.sender_id, m.sender_name, m.text, m.pinned, m.created_at "
              "FROM messages m JOIN messages_fts f ON m.id = f.rowid "
              "WHERE m.channel_id = ? AND m.deleted = 0 AND messages_fts MATCH ? "
              "ORDER BY m.id DESC LIMIT ?";
    } else {
        sql = "SELECT m.id, m.channel_id, m.sender_id, m.sender_name, m.text, m.pinned, m.created_at "
              "FROM messages m JOIN messages_fts f ON m.id = f.rowid "
              "WHERE m.channel_id = ? AND m.deleted = 0 AND m.id < ? AND messages_fts MATCH ? "
              "ORDER BY m.id DESC LIMIT ?";
    }

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    if (before_id == 0) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
        sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
    } else {
        sqlite3_bind_int(stmt, 1, static_cast<int>(channel_id));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(before_id));
        sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, limit);
    }

    std::vector<MessageRow> result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(read_message_row(stmt));

    sqlite3_finalize(stmt);
    return result;
}

// --- File attachments ---

uint64_t Database::insert_attachment(uint64_t message_id, const std::string& file_name,
                                      int64_t file_size, const std::string& mime_type,
                                      const std::string& disk_path) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO file_attachments (message_id, file_name, file_size, mime_type, disk_path) "
                      "VALUES (?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));
    sqlite3_bind_text(stmt, 2, file_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, file_size);
    sqlite3_bind_text(stmt, 4, mime_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, disk_path.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }

    auto id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
    sqlite3_finalize(stmt);
    return id;
}

bool Database::mark_attachment_uploaded(uint64_t attachment_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE file_attachments SET uploaded = 1 WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(attachment_id));
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<AttachmentRow> Database::get_attachment(uint64_t file_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, message_id, file_name, file_size, mime_type, disk_path, uploaded "
                      "FROM file_attachments WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(file_id));

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    AttachmentRow row;
    row.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    row.message_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    row.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.file_size = sqlite3_column_int64(stmt, 3);
    row.mime_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    row.disk_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    row.uploaded = sqlite3_column_int(stmt, 6) != 0;

    sqlite3_finalize(stmt);
    return row;
}

std::vector<AttachmentRow> Database::get_attachments_for_message(uint64_t message_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, message_id, file_name, file_size, mime_type, disk_path, uploaded "
                      "FROM file_attachments WHERE message_id = ? ORDER BY id";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(message_id));

    std::vector<AttachmentRow> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AttachmentRow row;
        row.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        row.message_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        row.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.file_size = sqlite3_column_int64(stmt, 3);
        row.mime_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        row.disk_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        row.uploaded = sqlite3_column_int(stmt, 6) != 0;
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return result;
}

// --- Retention ---

int Database::purge_old_messages(int days) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE messages SET deleted = 1 "
                      "WHERE deleted = 0 AND created_at < unixepoch() - ? * 86400";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, days);
    sqlite3_step(stmt);
    int count = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return count;
}

int Database::purge_old_files(int days, const std::string& storage_path) {
    // Get files to delete
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, disk_path FROM file_attachments "
                      "WHERE created_at < datetime('now', '-' || ? || ' days')";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, days);

    std::vector<std::pair<uint64_t, std::string>> files;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        auto path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        files.emplace_back(id, path);
    }
    sqlite3_finalize(stmt);

    int count = 0;
    for (auto& [id, path] : files) {
        std::filesystem::remove(std::filesystem::path(storage_path) / path);
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM file_attachments WHERE id = ?", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(del, 1, static_cast<sqlite3_int64>(id));
            sqlite3_step(del);
            sqlite3_finalize(del);
            count++;
        }
    }
    return count;
}

int64_t Database::get_total_file_size() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(SUM(file_size), 0) FROM file_attachments WHERE uploaded = 1";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return total;
}

int Database::purge_oldest_files(int64_t max_size, const std::string& storage_path) {
    int64_t total = get_total_file_size();
    if (total <= max_size) return 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, disk_path, file_size FROM file_attachments "
                      "WHERE uploaded = 1 ORDER BY id ASC";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && total > max_size) {
        auto id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        auto path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto size = sqlite3_column_int64(stmt, 2);

        std::filesystem::remove(std::filesystem::path(storage_path) / path);

        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM file_attachments WHERE id = ?", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(del, 1, static_cast<sqlite3_int64>(id));
            sqlite3_step(del);
            sqlite3_finalize(del);
        }

        total -= size;
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

// --- Admin ---

bool Database::has_any_users() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM users";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool has_users = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        has_users = sqlite3_column_int(stmt, 0) > 0;

    sqlite3_finalize(stmt);
    return has_users;
}

} // namespace parties::server
