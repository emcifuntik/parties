// Connection-security regression tests, over real MsQuic on loopback.
//
// Part 1 - the login is bound to the server (protocol 1.5).
//   Three servers run in this process: A and B with their own certificates
//   (both refuse the pre-1.5 login, the default) and C, which shares A's
//   certificate and runs with allow_legacy_auth.
//     1. A server challenges every connection with a fresh 32-byte nonce.
//     2. The pre-1.5 login is refused with BadVersion; the same signature
//        under a 1.5 version number is refused with BadAuth.
//     3. A login that answers another connection's challenge is refused.
//     4. Relay: a victim logs in to server A. Replaying that AUTH_IDENTITY to
//        server B is refused, and so is the stronger attack in which A hands
//        the victim B's challenge: the victim signs the certificate it saw
//        (A's), which is not B's. The same identity logs in to B normally.
//     5. With allow_legacy_auth the pre-1.5 login is accepted again.
//
// Part 2 - a file-transfer stream cannot make the server buffer data it did
// not agree to (QuicServer on its own, no main loop).
//     6. An unauthenticated session's upload stream is refused.
//     7. An authenticated session's upload is refused when the attachment was
//        not announced, was announced by another session, was announced with
//        a size above the server's limit, or carries more than announced.
//     8. An announced upload of the announced size is delivered intact, also
//        after an earlier refused attempt for the same attachment.
//     9. Uploads that exceed what a session may hold at once wait instead of
//        being refused, and all arrive.
//    10. A finished upload returns its stream: more uploads than the
//        connection's stream limit go through one after another.

#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/protocol.h>
#include <parties/quic_common.h>
#include <parties/serialization.h>
#include <parties/types.h>

#include <server/config.h>
#include <server/quic_server.h>
#include <server/server.h>

#include <client/net_client.h>

#include "test_auth.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace parties;
using namespace parties::protocol;
using namespace parties::client;
using namespace parties::server;
using parties::test::AuthNonce;

namespace {

constexpr uint16_t PORT_A      = 17850;   // default policy, certificate A
constexpr uint16_t PORT_B      = 17851;   // default policy, certificate B
constexpr uint16_t PORT_C      = 17852;   // allow_legacy_auth, certificate A
constexpr uint16_t PORT_FILES  = 17853;   // bare QuicServer
constexpr int      TIMEOUT_MS  = 10000;
constexpr uint16_t VERSION_1_4 = static_cast<uint16_t>((1u << 8) | 4u);

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } while (0)

int g_failures = 0;
#define CHECK(cond, what) do {                                        \
    if (!(cond)) { LOG("FAIL: %s (line %d)\n", what, __LINE__); ++g_failures; } \
    else         { LOG("  ok: %s\n", what); }                          \
} while (0)
#define REQUIRE(cond, what) do {                                      \
    if (!(cond)) { LOG("FAIL: %s (line %d)\n", what, __LINE__); return false; } \
} while (0)

bool wait_for_connect(NetClient& client) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.is_connected()) return true;
        if (client.connect_failed()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = TIMEOUT_MS) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// The server's answer to an AUTH_IDENTITY.
struct LoginResult {
    bool        answered = false;
    bool        accepted = false;              // AUTH_RESPONSE
    uint16_t    error = 0xFFFF;                // ServerErrorCode of a SERVER_ERROR
    std::string message;                       // its text
};

LoginResult send_login(NetClient& client, const std::vector<uint8_t>& auth_identity) {
    LoginResult result;
    if (auth_identity.empty() ||
        !client.send_message(ControlMessageType::AUTH_IDENTITY,
                             auth_identity.data(), auth_identity.size()))
        return result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto msg = client.incoming().try_pop()) {
            if (msg->type == ControlMessageType::AUTH_RESPONSE) {
                result.answered = result.accepted = true;
                return result;
            }
            if (msg->type == ControlMessageType::SERVER_ERROR) {
                BinaryReader r(msg->payload.data(), msg->payload.size());
                result.answered = true;
                result.error = r.read_u16();
                result.message = r.read_string();
                return result;
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return result;
}

bool refused_with(const LoginResult& r, ServerErrorCode code) {
    return r.answered && !r.accepted && r.error == static_cast<uint16_t>(code);
}

// Refused because the signature did not verify, and for no other reason
// (several refusals share the BadAuth code).
bool refused_signature(const LoginResult& r) {
    return refused_with(r, ServerErrorCode::BadAuth) && r.message == "Invalid signature";
}

struct Identity {
    SecretKey sk{};
    PublicKey pk{};
};

bool make_identity(const char* phrase, Identity& id) {
    return derive_keypair(phrase, id.sk, id.pk);
}

// A connected client with the server's challenge already read.
struct Peer {
    NetClient   client;
    AuthNonce   nonce{};
    std::string fingerprint;   // of the certificate seen in the handshake

    bool open(uint16_t port) {
        if (!client.connect("127.0.0.1", port) || !wait_for_connect(client)) return false;
        if (!test::wait_for_auth_challenge(client, nonce, TIMEOUT_MS)) return false;
        fingerprint = client.get_server_fingerprint();
        return !fingerprint.empty();
    }
};

struct RunningServer {
    Server      server;
    std::thread thread;

    bool start(const Config& cfg) {
        if (!server.start(cfg)) return false;
        thread = std::thread([this] { server.run(); });
        return true;
    }
    ~RunningServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

Config server_config(const fs::path& dir, const char* name, uint16_t port,
                     const fs::path& cert, const fs::path& key) {
    Config cfg;
    cfg.listen_ip = "127.0.0.1";
    cfg.port      = port;
    cfg.cert_file = cert.string();
    cfg.key_file  = key.string();
    cfg.db_path   = (dir / (std::string(name) + ".db")).string();
    cfg.chat.file_storage_path = (dir / (std::string(name) + "_files")).string();
    return cfg;
}

// ── Part 1 ───────────────────────────────────────────────────────────────

bool test_login_binding(const fs::path& dir) {
    LOG("\n[1] Login is bound to the server\n");

    const fs::path cert_a = dir / "a.pem", key_a = dir / "a.key.pem";
    const fs::path cert_b = dir / "b.pem", key_b = dir / "b.key.pem";

    RunningServer server_a, server_b, server_c;
    REQUIRE(server_a.start(server_config(dir, "a", PORT_A, cert_a, key_a)), "server A start");
    REQUIRE(server_b.start(server_config(dir, "b", PORT_B, cert_b, key_b)), "server B start");
    Config cfg_c = server_config(dir, "c", PORT_C, cert_a, key_a);   // A's certificate
    cfg_c.allow_legacy_auth = true;
    REQUIRE(server_c.start(cfg_c), "server C start");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Identity victim, old_client, other, legacy_ok;
    REQUIRE(make_identity("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", victim), "identity");
    REQUIRE(make_identity("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", old_client), "identity");
    REQUIRE(make_identity("letter advice cage absurd amount doctor acoustic avoid letter advice cage above", other), "identity");
    REQUIRE(make_identity("legal winner thank year wave sausage worth useful legal winner thank yellow", legacy_ok), "identity");

    // 1. Every connection gets its own challenge.
    Peer a1, a2;
    REQUIRE(a1.open(PORT_A), "first connection to A receives AUTH_CHALLENGE");
    REQUIRE(a2.open(PORT_A), "second connection to A receives AUTH_CHALLENGE");
    CHECK(a1.nonce != a2.nonce, "two connections get different challenges");
    CHECK(a1.fingerprint == a2.fingerprint, "both connections see A's certificate");

    // 2. The pre-1.5 login.
    CHECK(refused_with(send_login(a1.client,
              test::legacy_auth_identity(old_client.sk, old_client.pk, "old", VERSION_1_4)),
              ServerErrorCode::BadVersion),
          "a 1.4 client's unbound login is refused with BadVersion");
    CHECK(refused_signature(send_login(a1.client,
              test::legacy_auth_identity(old_client.sk, old_client.pk, "old", PROTOCOL_VERSION))),
          "the unbound signature under a 1.5 version number does not verify");

    // 3. A login made for another connection of the same server.
    CHECK(refused_signature(send_login(a2.client,
              test::bound_auth_identity(a1.fingerprint, a1.nonce, other.sk, other.pk, "other"))),
          "a login that answers another connection's challenge does not verify");
    CHECK(send_login(a2.client,
              test::bound_auth_identity(a2.fingerprint, a2.nonce, other.sk, other.pk, "other")).accepted,
          "the same identity logs in with this connection's challenge");

    // 4. Relay from server A to server B.
    Peer victim_at_a;                      // the victim's connection to the hostile server A
    Peer relay_at_b;                       // A's operator, connected to B as a client
    REQUIRE(victim_at_a.open(PORT_A), "victim connects to A");
    REQUIRE(relay_at_b.open(PORT_B), "relay connects to B");
    CHECK(victim_at_a.fingerprint != relay_at_b.fingerprint, "A and B have different certificates");

    const std::vector<uint8_t> login_for_a = test::bound_auth_identity(
        victim_at_a.fingerprint, victim_at_a.nonce, victim.sk, victim.pk, "victim");
    CHECK(send_login(victim_at_a.client, login_for_a).accepted, "victim logs in to A");
    CHECK(refused_signature(send_login(relay_at_b.client, login_for_a)),
          "the victim's login for A, replayed to B, does not verify");

    // A relays B's challenge to the victim instead of its own. The victim
    // still signs the certificate it saw: A's.
    const std::vector<uint8_t> login_with_b_nonce = test::bound_auth_identity(
        victim_at_a.fingerprint, relay_at_b.nonce, victim.sk, victim.pk, "victim");
    CHECK(refused_signature(send_login(relay_at_b.client, login_with_b_nonce)),
          "a login answering B's challenge but signed for A's certificate does not verify at B");

    Peer victim_at_b;
    REQUIRE(victim_at_b.open(PORT_B), "victim connects to B");
    CHECK(send_login(victim_at_b.client, test::bound_auth_identity(
              victim_at_b.fingerprint, victim_at_b.nonce, victim.sk, victim.pk, "victim")).accepted,
          "the victim itself logs in to B");

    // 5. allow_legacy_auth.
    Peer c1;
    REQUIRE(c1.open(PORT_C), "a server with allow_legacy_auth still sends AUTH_CHALLENGE");
    CHECK(send_login(c1.client,
              test::legacy_auth_identity(legacy_ok.sk, legacy_ok.pk, "legacy", VERSION_1_4)).accepted,
          "with allow_legacy_auth a 1.4 client's login is accepted");
    return true;
}

// ── Part 2 ───────────────────────────────────────────────────────────────

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(seed + i * 31);
    return v;
}

bool test_file_streams(const fs::path& dir) {
    LOG("\n[2] File-transfer streams are admitted before they are buffered\n");

    constexpr int64_t MAX_UPLOAD = 64 * 1024;
    QuicServer quic;
    quic.set_max_upload_bytes(MAX_UPLOAD);
    // Reuses server A's certificate from part 1.
    REQUIRE(quic.start("127.0.0.1", PORT_FILES, 8,
                       (dir / "a.pem").string(), (dir / "a.key.pem").string()),
            "QuicServer start");

    Peer alice, bob;
    REQUIRE(alice.open(PORT_FILES), "alice connects");
    REQUIRE(wait_until([&] { return quic.get_sessions().size() == 1; }), "alice's session exists");
    const std::shared_ptr<Session> alice_session = quic.get_sessions().front();
    REQUIRE(bob.open(PORT_FILES), "bob connects");
    REQUIRE(wait_until([&] { return quic.get_sessions().size() == 2; }), "bob's session exists");
    std::shared_ptr<Session> bob_session;
    for (auto& s : quic.get_sessions())
        if (s->id != alice_session->id) bob_session = s;
    REQUIRE(bob_session != nullptr, "bob's session found");

    uint64_t refused = quic.file_streams_refused();
    auto refused_once_more = [&](const char* what) {
        const bool ok = wait_until([&] { return quic.file_streams_refused() == refused + 1; });
        refused = quic.file_streams_refused();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(ok && !quic.file_uploads().try_pop().has_value(), what);
    };

    // 6. Not authenticated: refused although the upload is announced and has
    // exactly the announced size, so nothing but the missing login can be the
    // reason.
    const std::vector<uint8_t> small = pattern(1000, 7);
    quic.expect_upload(1, alice_session->id, static_cast<int64_t>(small.size()));
    REQUIRE(alice.client.upload_file(1, small.data(), small.size()), "send");
    refused_once_more("an unauthenticated session's upload stream is refused");

    // What the server's main loop does on a successful login.
    alice_session->file_streams_allowed.store(true, std::memory_order_release);
    bob_session->file_streams_allowed.store(true, std::memory_order_release);

    // The same upload goes through once the session is authenticated.
    REQUIRE(alice.client.upload_file(1, small.data(), small.size()), "send");
    {
        std::optional<FileUploadEvent> first;
        const bool ok = wait_until([&] { first = quic.file_uploads().try_pop(); return first.has_value(); });
        CHECK(ok && first->attachment_id == 1 && first->data == small,
              "and is accepted once the session is authenticated");
    }

    // 7. Authenticated, but not entitled.
    REQUIRE(alice.client.upload_file(2, small.data(), small.size()), "send");
    refused_once_more("an upload that was never announced is refused");

    quic.expect_upload(3, bob_session->id, 1000);
    REQUIRE(alice.client.upload_file(3, small.data(), small.size()), "send");
    refused_once_more("an upload announced by another session is refused");

    quic.expect_upload(4, alice_session->id, MAX_UPLOAD + 1);
    const std::vector<uint8_t> over_limit = pattern(static_cast<size_t>(MAX_UPLOAD) + 1, 3);
    REQUIRE(alice.client.upload_file(4, over_limit.data(), over_limit.size()), "send");
    refused_once_more("an upload announced above the server's size limit is refused");

    quic.expect_upload(5, alice_session->id, 1000);
    const std::vector<uint8_t> too_long = pattern(4000, 9);
    REQUIRE(alice.client.upload_file(5, too_long.data(), too_long.size()), "send");
    refused_once_more("an upload longer than its announced size is refused");

    const std::vector<uint8_t> too_short = pattern(400, 11);
    REQUIRE(alice.client.upload_file(5, too_short.data(), too_short.size()), "send");
    refused_once_more("an upload shorter than its announced size is dropped");

    // 8. The announced upload goes through, after the refused attempts above.
    REQUIRE(alice.client.upload_file(5, small.data(), small.size()), "send");
    std::optional<FileUploadEvent> ev;
    const bool delivered = wait_until([&] {
        ev = quic.file_uploads().try_pop();
        return ev.has_value();
    });
    CHECK(delivered && ev->session_id == alice_session->id && ev->attachment_id == 5 &&
          ev->data == small,
          "an announced upload of the announced size is delivered intact");
    CHECK(quic.file_streams_refused() == refused, "and it is not counted as a refusal");

    REQUIRE(alice.client.upload_file(5, small.data(), small.size()), "send");
    refused_once_more("a completed upload cannot be sent a second time");

    // 9. Uploads that do not fit at once are not refused: they wait. Three
    // files of 48 KiB exceed what one session may hold (one largest file,
    // 64 KiB), so the second and third are held back by flow control until
    // the one before them is done.
    const uint64_t refused_before_wait = quic.file_streams_refused();
    std::vector<std::vector<uint8_t>> files;
    for (uint8_t i = 0; i < 3; i++) {
        files.push_back(pattern(48 * 1024, static_cast<uint8_t>(40 + i)));
        quic.expect_upload(20 + i, alice_session->id, static_cast<int64_t>(files[i].size()));
    }
    for (uint8_t i = 0; i < 3; i++)
        REQUIRE(alice.client.upload_file(20 + i, files[i].data(), files[i].size()), "send");
    int intact = 0;
    for (int n = 0; n < 3; n++) {
        std::optional<FileUploadEvent> got;
        if (!wait_until([&] { got = quic.file_uploads().try_pop(); return got.has_value(); })) break;
        if (got->attachment_id >= 20 && got->attachment_id < 23 &&
            got->data == files[got->attachment_id - 20])
            ++intact;
    }
    CHECK(intact == 3, "three uploads that exceed the per-session reservation all arrive intact");
    CHECK(quic.file_streams_refused() == refused_before_wait, "none of them was refused");

    // 10. A finished upload gives its stream back: more uploads than the
    // connection may have file streams open (8) go through one after another.
    int sequential = 0;
    for (uint8_t i = 0; i < 12; i++) {
        const std::vector<uint8_t> file = pattern(1024, static_cast<uint8_t>(90 + i));
        quic.expect_upload(30 + i, alice_session->id, static_cast<int64_t>(file.size()));
        REQUIRE(alice.client.upload_file(30 + i, file.data(), file.size()), "send");
        std::optional<FileUploadEvent> got;
        if (!wait_until([&] { got = quic.file_uploads().try_pop(); return got.has_value(); }, 3000))
            break;
        if (got->attachment_id == 30u + i && got->data == file) ++sequential;
    }
    LOG("  sequential uploads delivered: %d of 12\n", sequential);
    CHECK(sequential == 12, "twelve uploads in a row on one connection all arrive");

    alice.client.disconnect();
    bob.client.disconnect();
    quic.stop();
    return true;
}

} // namespace

int main() {
    LOG("=== Parties connection security test ===\n");
    if (!crypto_init() || !net_init() || quic_init() == nullptr) {
        LOG("FAIL: subsystem init\n");
        return 1;
    }

    const fs::path dir = fs::temp_directory_path() / "parties_connection_security_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    bool ran = test_login_binding(dir);
    ran = ran && test_file_streams(dir);

    quic_cleanup();
    net_cleanup();
    crypto_cleanup();
    std::error_code ec;
    fs::remove_all(dir, ec);

    if (!ran || g_failures != 0) {
        LOG("\n=== FAILED (%d check(s)) ===\n", g_failures);
        return 1;
    }
    LOG("\n=== PASSED ===\n");
    return 0;
}
