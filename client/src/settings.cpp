#include <client/settings.h>
#include <parties/crypto.h>

#include <parties/log.h>

#include <sqlite3.h>
#include <cstring>

namespace parties::client {

Settings::Settings() = default;

Settings::~Settings() {
    close();
}

bool Settings::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("Failed to open {}: {}", path, sqlite3_errmsg(db_));
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
        LOG_ERROR("SQL error: {}", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Settings::create_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS identity (
            id          INTEGER PRIMARY KEY CHECK (id = 1),
            seed_phrase TEXT NOT NULL,
            public_key  BLOB NOT NULL,
            secret_key  BLOB NOT NULL
        );

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
            password      TEXT NOT NULL DEFAULT '',
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

    if (!exec(schema)) return false;

    // Migration: add password only to databases created before this column existed.
    // SQLite has no portable `ADD COLUMN IF NOT EXISTS`, so inspect the schema
    // first instead of treating "duplicate column" as an expected error.
    sqlite3_stmt* table_info = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(saved_servers)", -1,
                           &table_info, nullptr) != SQLITE_OK) {
        LOG_ERROR("Failed to inspect saved_servers schema: {}", sqlite3_errmsg(db_));
        return false;
    }

    bool has_password = false;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(table_info)) == SQLITE_ROW) {
        const unsigned char* column_name = sqlite3_column_text(table_info, 1);
        if (column_name && std::strcmp(reinterpret_cast<const char*>(column_name), "password") == 0) {
            has_password = true;
            break;
        }
    }
    sqlite3_finalize(table_info);

    if (!has_password && step_result != SQLITE_DONE) {
        LOG_ERROR("Failed to read saved_servers schema: {}", sqlite3_errmsg(db_));
        return false;
    }
    if (!has_password &&
        !exec("ALTER TABLE saved_servers ADD COLUMN password TEXT NOT NULL DEFAULT ''"))
        return false;

    return true;
}

// --- Identity ---

bool Settings::has_identity() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM identity";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool has = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        has = sqlite3_column_int(stmt, 0) > 0;

    sqlite3_finalize(stmt);
    return has;
}

bool Settings::save_identity(const std::string& seed_phrase,
                              const SecretKey& sk, const PublicKey& pk) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO identity (id, seed_phrase, public_key, secret_key) "
                      "VALUES (1, ?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, seed_phrase.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, pk.data(), static_cast<int>(pk.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, sk.data(), static_cast<int>(sk.size()), SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void Settings::delete_identity() {
    exec("DELETE FROM identity WHERE id = 1");
}

std::optional<Identity> Settings::load_identity() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT seed_phrase, public_key, secret_key FROM identity WHERE id = 1";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Identity id;
    id.seed_phrase = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

    auto* pk_blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
    int pk_len = sqlite3_column_bytes(stmt, 1);
    if (pk_blob && pk_len == 32)
        std::memcpy(id.public_key.data(), pk_blob, 32);

    auto* sk_blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
    int sk_len = sqlite3_column_bytes(stmt, 2);
    if (sk_blob && sk_len == 32)
        std::memcpy(id.secret_key.data(), sk_blob, 32);

    sqlite3_finalize(stmt);
    return id;
}

std::string Settings::get_fingerprint() {
    auto id = load_identity();
    if (!id) return "";
    return parties::public_key_fingerprint(id->public_key);
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
                           const std::string& password) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO saved_servers (name, host, port, fingerprint, last_username, password) "
                      "VALUES (?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(host, port) DO UPDATE SET "
                      "name=excluded.name, fingerprint=excluded.fingerprint, "
                      "last_username=excluded.last_username, password=excluded.password";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_text(stmt, 4, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, last_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, password.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<SavedServer> Settings::get_saved_servers() {
    std::vector<SavedServer> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, host, port, fingerprint, last_username, password "
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
        s.password = pw ? reinterpret_cast<const char*>(pw) : "";
        result.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Settings::update_server_username(int id, const std::string& username) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE saved_servers SET last_username = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
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
