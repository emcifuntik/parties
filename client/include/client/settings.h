#pragma once

#include <parties/types.h>

#include <string>
#include <vector>
#include <optional>

struct sqlite3;

namespace parties::client {

struct SavedServer {
    int         id = 0;
    std::string name;
    std::string host;
    int         port = 7800;
    std::string fingerprint;   // TOFU cert fingerprint
    std::string last_username;
};

struct Identity {
    std::string seed_phrase;
    SecretKey   secret_key{};
    PublicKey   public_key{};
};

class Settings {
public:
    Settings();
    ~Settings();

    bool open(const std::string& path);
    void close();

    // --- Identity ---
    bool has_identity();
    bool save_identity(const std::string& seed_phrase,
                       const SecretKey& sk, const PublicKey& pk);
    std::optional<Identity> load_identity();
    std::string get_fingerprint();

    // --- TOFU certificate store ---
    enum class TofuResult { Trusted, Unknown, Mismatch };
    TofuResult check_fingerprint(const std::string& host, int port,
                                  const std::string& fingerprint);
    bool trust_fingerprint(const std::string& host, int port,
                           const std::string& fingerprint);

    // --- Saved servers ---
    bool save_server(const std::string& name, const std::string& host, int port,
                     const std::string& fingerprint, const std::string& last_username);
    std::vector<SavedServer> get_saved_servers();
    bool delete_server(int id);

    // --- Resumption tickets (0-RTT) ---
    bool save_resumption_ticket(const std::string& host, int port,
                                const uint8_t* data, size_t len);
    std::vector<uint8_t> load_resumption_ticket(const std::string& host, int port);
    bool delete_resumption_ticket(const std::string& host, int port);

    // --- Preferences ---
    bool set_pref(const std::string& key, const std::string& value);
    std::optional<std::string> get_pref(const std::string& key);

private:
    bool exec(const std::string& sql);
    bool create_schema();

    sqlite3* db_ = nullptr;
};

} // namespace parties::client
