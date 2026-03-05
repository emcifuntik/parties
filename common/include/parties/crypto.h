#pragma once

#include <parties/types.h>

#include <string>
#include <cstdint>

namespace parties {

// Initialize/cleanup wolfCrypt globally (call once)
bool crypto_init();
void crypto_cleanup();

// Generate cryptographically random bytes
void random_bytes(uint8_t* out, size_t len);

// Compute SHA-256 hash, return hex-with-colons string
std::string sha256_hex(const uint8_t* data, size_t len);

// Generate a self-signed RSA 4096 certificate + private key, write PEM files
bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path);

// --- Seed phrase identity ---

// Generate 12-word seed phrase from embedded BIP-39 wordlist
std::string generate_seed_phrase();

// Validate seed phrase: 12 words, all in wordlist
bool validate_seed_phrase(const std::string& seed_phrase);

// Derive Ed25519 keypair from seed phrase (SHA-256 → 32-byte seed → keypair)
bool derive_keypair(const std::string& seed_phrase, SecretKey& sk, PublicKey& pk);

// Ed25519 sign
bool ed25519_sign(const uint8_t* msg, size_t msg_len,
                  const SecretKey& sk, const PublicKey& pk,
                  Signature& sig_out);

// Ed25519 verify
bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const Signature& sig, const PublicKey& pk);

// Fingerprint: SHA-256 of public key, colon-separated hex
Fingerprint public_key_fingerprint(const PublicKey& pk);

} // namespace parties
