#pragma once

// Test helpers: the login exchange of protocol 1.5 (server challenge, then an
// AUTH_IDENTITY whose signature is bound to the server's certificate and the
// challenge), plus the pre-1.5 login for the cases that exercise it.

#include <client/net_client.h>

#include <parties/auth_message.h>
#include <parties/crypto.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/types.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace parties::test {

using AuthNonce = std::array<uint8_t, protocol::AUTH_NONCE_SIZE>;

// Waits for the server's AUTH_CHALLENGE, the first message on a new
// connection. Anything else that arrives meanwhile is discarded.
inline bool wait_for_auth_challenge(client::NetClient& client, AuthNonce& nonce,
                                    int timeout_ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto msg = client.incoming().try_pop()) {
            if (msg->type == protocol::ControlMessageType::AUTH_CHALLENGE &&
                msg->payload.size() >= nonce.size()) {
                std::memcpy(nonce.data(), msg->payload.data(), nonce.size());
                return true;
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// AUTH_IDENTITY: [version(2)][pubkey(32)][name][timestamp(8)][signature(64)][password]
inline std::vector<uint8_t> auth_identity_payload(uint16_t version, const PublicKey& pk,
                                                  const std::string& name, uint64_t timestamp,
                                                  const Signature& sig,
                                                  const std::string& password = {}) {
    BinaryWriter w;
    w.write_u16(version);
    w.write_bytes(pk.data(), pk.size());
    w.write_string(name);
    w.write_u64(timestamp);
    w.write_bytes(sig.data(), sig.size());
    w.write_string(password);
    return w.data();
}

// What a 1.5 client sends: the signature covers `server_fingerprint` and
// `nonce`. Empty on failure.
inline std::vector<uint8_t> bound_auth_identity(const std::string& server_fingerprint,
                                                const AuthNonce& nonce,
                                                const SecretKey& sk, const PublicKey& pk,
                                                const std::string& name,
                                                uint16_t version = protocol::PROTOCOL_VERSION) {
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    const std::vector<uint8_t> msg =
        protocol::auth_signed_message_bound(server_fingerprint, nonce.data(), pk, name, now);
    Signature sig{};
    if (!ed25519_sign(msg.data(), msg.size(), sk, pk, sig)) return {};
    return auth_identity_payload(version, pk, name, now, sig);
}

// What a client older than 1.5 sends: the signature names no server.
inline std::vector<uint8_t> legacy_auth_identity(const SecretKey& sk, const PublicKey& pk,
                                                 const std::string& name, uint16_t version) {
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    const std::vector<uint8_t> msg = protocol::auth_signed_message_legacy(pk, name, now);
    Signature sig{};
    if (!ed25519_sign(msg.data(), msg.size(), sk, pk, sig)) return {};
    return auth_identity_payload(version, pk, name, now, sig);
}

// Waits for the challenge and sends the 1.5 login for the certificate this
// client saw in its handshake. The caller then waits for AUTH_RESPONSE.
inline bool send_bound_auth(client::NetClient& client, const SecretKey& sk, const PublicKey& pk,
                            const std::string& name, int timeout_ms = 10000) {
    AuthNonce nonce{};
    if (!wait_for_auth_challenge(client, nonce, timeout_ms)) return false;
    const std::vector<uint8_t> payload =
        bound_auth_identity(client.get_server_fingerprint(), nonce, sk, pk, name);
    return !payload.empty() &&
           client.send_message(protocol::ControlMessageType::AUTH_IDENTITY,
                               payload.data(), payload.size());
}

} // namespace parties::test
