#pragma once

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

} // namespace parties
