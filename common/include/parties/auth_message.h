#pragma once

#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/types.h>

#include <cstdint>
#include <string>
#include <vector>

// The bytes an identity signs in AUTH_IDENTITY. Shared by the client, the
// server and the tests so the three can never disagree on the layout.

namespace parties::protocol {

// Legacy (clients older than 1.5): pubkey, display name, timestamp. Nothing in
// it names the server it was made for, so any server a user logs in to can
// replay the whole AUTH_IDENTITY to another server inside the freshness window
// and act as that user there. A 1.5 server accepts it only when the operator
// opted in (Config::allow_legacy_auth); a 1.5 client never signs it.
inline std::vector<uint8_t> auth_signed_message_legacy(const PublicKey& pubkey,
                                                       const std::string& display_name,
                                                       uint64_t timestamp) {
    BinaryWriter w;
    w.write_bytes(pubkey.data(), pubkey.size());
    w.write_string(display_name);
    w.write_u64(timestamp);
    return w.data();
}

// Domain separation: no other message the identity key signs starts with it,
// and a legacy message (which starts with the signer's public key) can never
// be mistaken for a bound one.
inline constexpr char AUTH_SIGNING_CONTEXT[] = "parties-auth-1.5";

// Bound (1.5+): additionally covers
//   - the fingerprint of the server's TLS certificate (sha256_hex of the leaf
//     DER, the same string the client pins). The client signs the certificate
//     it saw in the handshake; the server verifies with its own. A server that
//     relays the login elsewhere fails, because the other server's
//     certificate differs and the relay cannot present a certificate it has no
//     key for;
//   - the nonce from AUTH_CHALLENGE, fresh per connection, so a captured login
//     is useless on any later connection.
inline std::vector<uint8_t> auth_signed_message_bound(const std::string& server_cert_fingerprint,
                                                      const uint8_t* nonce,   // AUTH_NONCE_SIZE bytes
                                                      const PublicKey& pubkey,
                                                      const std::string& display_name,
                                                      uint64_t timestamp) {
    BinaryWriter w;
    w.write_string(AUTH_SIGNING_CONTEXT);
    w.write_string(server_cert_fingerprint);
    w.write_bytes(nonce, AUTH_NONCE_SIZE);
    w.write_bytes(pubkey.data(), pubkey.size());
    w.write_string(display_name);
    w.write_u64(timestamp);
    return w.data();
}

} // namespace parties::protocol
