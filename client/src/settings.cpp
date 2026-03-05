#include <client/settings.h>

#include <sqlite3.h>
#include <cstdio>

namespace parties::client {

Settings::Settings() = default;

Settings::~Settings() {
    close();
}

bool Settings::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::fprintf(stderr, "[Settings] Failed to open %s: %s\n",
                     path.c_str(), sqlite3_errmsg(db_));
        return false;
    }

    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");

    if (!create_schema()) {
        close();
        return false;
    }

    return true;
}

void Settings::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Settings::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "[Settings] SQL error: %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Settings::create_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS tofu_certs (
            host        TEXT NOT NULL,
            port        INTEGER NOT NULL,
            fingerprint TEXT NOT NULL,
            first_seen  TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (host, port)
        );

        CREATE TABLE IF NOT EXISTS saved_servers (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            name          TEXT NOT NULL,
            host          TEXT NOT NULL,
            port          INTEGER NOT NULL DEFAULT 7800,
            fingerprint   TEXT NOT NULL DEFAULT '',
            last_username TEXT NOT NULL DEFAULT '',
            UNIQUE(host, port)
        );

        CREATE TABLE IF NOT EXISTS preferences (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS resumption_tickets (
            host     TEXT NOT NULL,
            port     INTEGER NOT NULL,
            ticket   BLOB NOT NULL,
            saved_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (host, port)
        );
    )SQL";

    if (!exec(schema))
        return false;

    // Migration: add last_password column if it doesn't exist (ignores error if already present)
    exec("ALTER TABLE saved_servers ADD COLUMN last_password TEXT NOT NULL DEFAULT ''");

    return true;
}

// --- TOFU ---

Settings::TofuResult Settings::check_fingerprint(const std::string& host, int port,
                                                   const std::string& fingerprint) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT fingerprint FROM tofu_certs WHERE host = ? AND port = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return TofuResult::Unknown;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return TofuResult::Unknown;
    }

    std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    return (stored == fingerprint) ? TofuResult::Trusted : TofuResult::Mismatch;
}

bool Settings::trust_fingerprint(const std::string& host, int port,
                                  const std::string& fingerprint) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO tofu_certs (host, port, fingerprint) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 3, fingerprint.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Saved servers ---

bool Settings::save_server(const std::string& name, const std::string& host, int port,
                           const std::string& fingerprint, const std::string& last_username,
                           const std::string& last_password) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO saved_servers "
                      "(name, host, port, fingerprint, last_username, last_password) VALUES (?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_text(stmt, 4, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, last_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, last_password.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<SavedServer> Settings::get_saved_servers() {
    std::vector<SavedServer> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, host, port, fingerprint, last_username, last_password "
                      "FROM saved_servers ORDER BY name";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedServer s;
        s.id = sqlite3_column_int(stmt, 0);
        s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.host = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.port = sqlite3_column_int(stmt, 3);
        s.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        s.last_username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        auto pw = sqlite3_column_text(stmt, 6);
        if (pw) s.last_password = reinterpret_cast<const char*>(pw);
        result.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Settings::delete_server(int id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM saved_servers WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Resumption tickets ---

bool Settings::save_resumption_ticket(const std::string& host, int port,
                                       const uint8_t* data, size_t len) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO resumption_tickets (host, port, ticket) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_blob(stmt, 3, data, static_cast<int>(len), SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<uint8_t> Settings::load_resumption_ticket(const std::string& host, int port) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT ticket FROM resumption_tickets "
                      "WHERE host = ? AND port = ? "
                      "AND saved_at > datetime('now', '-24 hours')";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return {};
    }

    auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
    int blob_len = sqlite3_column_bytes(stmt, 0);
    std::vector<uint8_t> result(blob, blob + blob_len);
    sqlite3_finalize(stmt);
    return result;
}

bool Settings::delete_resumption_ticket(const std::string& host, int port) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM resumption_tickets WHERE host = ? AND port = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Preferences ---

bool Settings::set_pref(const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO preferences (key, value) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<std::string> Settings::get_pref(const std::string& key) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM preferences WHERE key = ?";

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

} // namespace parties::client
