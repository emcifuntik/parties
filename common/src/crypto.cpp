#include <parties/crypto.h>
#include <parties/bip39_wordlist.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace parties {

static bool g_initialized = false;

bool crypto_init() {
    if (g_initialized) return true;
    g_initialized = true;
    return true;
}

void crypto_cleanup() {
    if (!g_initialized) return;
    g_initialized = false;
}

void random_bytes(uint8_t* out, size_t len) {
    RAND_bytes(out, static_cast<int>(len));
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(data, len, hash);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(SHA256_DIGEST_LENGTH * 3);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        if (i > 0) result += ':';
        result += hex[(hash[i] >> 4) & 0xF];
        result += hex[hash[i] & 0xF];
    }
    return result;
}

bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path) {
    // Generate RSA 4096 key
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) return false;

    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 4096) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // Create self-signed X509 certificate
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 3650L * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("Parties"), -1, -1, 0);
    X509_set_issuer_name(x509, name);

    if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }

    // Write cert PEM
    FILE* f = fopen(cert_path.c_str(), "wb");
    if (!f) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_X509(f, x509);
    fclose(f);

    // Write key PEM
    f = fopen(key_path.c_str(), "wb");
    if (!f) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return true;
}

// --- Seed phrase identity ---

std::string generate_seed_phrase() {
    std::string phrase;
    for (int i = 0; i < 12; i++) {
        uint16_t idx;
        random_bytes(reinterpret_cast<uint8_t*>(&idx), sizeof(idx));
        idx %= 2048;
        if (i > 0) phrase += ' ';
        phrase += bip39_wordlist[idx];
    }
    return phrase;
}

bool validate_seed_phrase(const std::string& seed_phrase) {
    std::istringstream iss(seed_phrase);
    std::string word;
    int count = 0;
    while (iss >> word) {
        bool found = false;
        for (auto w : bip39_wordlist) {
            if (w == word) { found = true; break; }
        }
        if (!found) return false;
        count++;
    }
    return count == 12;
}

bool derive_keypair(const std::string& seed_phrase, SecretKey& sk, PublicKey& pk) {
    // SHA-256 of seed phrase -> 32-byte Ed25519 seed
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(seed_phrase.data()),
           seed_phrase.size(), hash);
    std::memcpy(sk.data(), hash, 32);

    // Create Ed25519 key from raw 32-byte seed
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                    sk.data(), 32);
    if (!pkey) return false;

    // Extract public key
    size_t pub_len = 32;
    if (EVP_PKEY_get_raw_public_key(pkey, pk.data(), &pub_len) != 1) {
        EVP_PKEY_free(pkey);
        return false;
    }

    EVP_PKEY_free(pkey);
    return true;
}

bool derive_pubkey(const SecretKey& sk, PublicKey& pk) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                    sk.data(), 32);
    if (!pkey) return false;

    size_t pub_len = 32;
    int ret = EVP_PKEY_get_raw_public_key(pkey, pk.data(), &pub_len);
    EVP_PKEY_free(pkey);
    return ret == 1;
}

std::string secret_key_to_hex(const SecretKey& sk) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto b : sk) {
        result += hex[(b >> 4) & 0xF];
        result += hex[b & 0xF];
    }
    return result;
}

bool secret_key_from_hex(const std::string& hex, SecretKey& sk) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        auto hi = hex[i * 2];
        auto lo = hex[i * 2 + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) return false;
        sk[i] = static_cast<uint8_t>((h << 4) | l);
    }
    return true;
}

bool ed25519_sign(const uint8_t* msg, size_t msg_len,
                  const SecretKey& sk, const PublicKey& pk,
                  Signature& sig_out) {
    (void)pk;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                    sk.data(), 32);
    if (!pkey) return false;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    size_t sig_len = 64;
    bool ok = EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestSign(md_ctx, sig_out.data(), &sig_len, msg, msg_len) == 1;

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const Signature& sig, const PublicKey& pk) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                   pk.data(), 32);
    if (!pkey) return false;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestVerify(md_ctx, sig.data(), 64, msg, msg_len) == 1;

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

Fingerprint public_key_fingerprint(const PublicKey& pk) {
    return sha256_hex(pk.data(), pk.size());
}

} // namespace parties
