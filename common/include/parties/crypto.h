#pragma once

#include <parties/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace parties {

[[nodiscard]] bool random_bytes(uint8_t* out, size_t len);

class SecretString {
public:
    SecretString() = default;
    explicit SecretString(std::string value) : value_(std::move(value)) {}

    bool empty() const { return value_.empty(); }
    [[nodiscard]] bool matches(std::string_view candidate) const;

private:
    std::string value_;
};

std::string sha256_colon_hex(const uint8_t* data, size_t len);

bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path);

[[nodiscard]] std::optional<std::string> generate_seed_phrase();

bool validate_seed_phrase(const std::string& seed_phrase);

[[nodiscard]] SecretKey secret_key_from_phrase_sha256(std::string_view seed_phrase);

[[nodiscard]] std::optional<PublicKey> public_key_of(const SecretKey& sk);

std::string secret_key_to_hex(const SecretKey& sk);
bool secret_key_from_hex(const std::string& hex, SecretKey& sk);

bool ed25519_sign(const uint8_t* msg, size_t msg_len,
                  const SecretKey& sk, const PublicKey& pk,
                  Signature& sig_out);

bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const Signature& sig, const PublicKey& pk);

Fingerprint public_key_fingerprint(const PublicKey& pk);

}
