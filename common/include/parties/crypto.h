#pragma once

#include <parties/types.h>

#include <string>
#include <cstdint>

namespace parties {

bool crypto_init();
void crypto_cleanup();

bool random_bytes(uint8_t* out, size_t len);

bool constant_time_equals(const std::string& a, const std::string& b);

std::string sha256_hex(const uint8_t* data, size_t len);

bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path);

std::string generate_seed_phrase();

bool validate_seed_phrase(const std::string& seed_phrase);

bool derive_keypair(const std::string& seed_phrase, SecretKey& sk, PublicKey& pk);

bool derive_pubkey(const SecretKey& sk, PublicKey& pk);

std::string secret_key_to_hex(const SecretKey& sk);
bool secret_key_from_hex(const std::string& hex, SecretKey& sk);

bool ed25519_sign(const uint8_t* msg, size_t msg_len,
                  const SecretKey& sk, const PublicKey& pk,
                  Signature& sig_out);

bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const Signature& sig, const PublicKey& pk);

Fingerprint public_key_fingerprint(const PublicKey& pk);

}
