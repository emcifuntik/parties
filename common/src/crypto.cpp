#include <parties/crypto.h>
#include <parties/bip39_wordlist.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/ed25519.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace parties {

static WC_RNG g_rng;
static bool g_initialized = false;

bool crypto_init() {
    if (g_initialized) return true;
    wolfCrypt_Init();
    if (wc_InitRng(&g_rng) != 0) return false;
    g_initialized = true;
    return true;
}

void crypto_cleanup() {
    if (!g_initialized) return;
    wc_FreeRng(&g_rng);
    wolfCrypt_Cleanup();
    g_initialized = false;
}

void random_bytes(uint8_t* out, size_t len) {
    wc_RNG_GenerateBlock(&g_rng, out, static_cast<word32>(len));
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    uint8_t hash[WC_SHA256_DIGEST_SIZE];
    wc_Sha256Hash(data, static_cast<word32>(len), hash);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(WC_SHA256_DIGEST_SIZE * 3);
    for (int i = 0; i < WC_SHA256_DIGEST_SIZE; i++) {
        if (i > 0) result += ':';
        result += hex[(hash[i] >> 4) & 0xF];
        result += hex[hash[i] & 0xF];
    }
    return result;
}

bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path) {
    // Heap-allocate RsaKey and Cert — both are very large structs in wolfSSL
    // (Cert alone can be 20KB+ with altNames) and will overflow the stack.
    auto* rsa = new RsaKey;
    wc_InitRsaKey(rsa, nullptr);

    // Generate RSA 4096 key
    if (wc_MakeRsaKey(rsa, 4096, WC_RSA_EXPONENT, &g_rng) != 0) {
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    // Buffer size for DER/PEM conversions — must be large enough for RSA 4096
    static constexpr word32 BUF_SZ = 16384;

    // Export private key to DER
    std::vector<uint8_t> key_der(BUF_SZ);
    int key_der_len = wc_RsaKeyToDer(rsa, key_der.data(), static_cast<word32>(key_der.size()));
    if (key_der_len <= 0) {
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    // Create certificate
    auto* cert = new Cert;
    wc_InitCert(cert);
    std::snprintf(cert->subject.commonName, CTC_NAME_SIZE, "%s", common_name.c_str());
    std::snprintf(cert->subject.org, CTC_NAME_SIZE, "%s", "Parties");
    cert->isCA = 0;
    cert->sigType = CTC_SHA256wRSA;
    cert->daysValid = 3650;

    // Use two-step approach: MakeCert + SignCert (more reliable than MakeSelfCert)
    std::vector<uint8_t> cert_der(BUF_SZ);

    int cert_body_len = wc_MakeCert(cert, cert_der.data(), BUF_SZ,
                                     rsa, nullptr, &g_rng);
    if (cert_body_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_MakeCert failed: %d\n", cert_body_len);
        delete cert;
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    int cert_der_len = wc_SignCert(cert->bodySz, cert->sigType,
                                    cert_der.data(), BUF_SZ,
                                    rsa, nullptr, &g_rng);
    delete cert;

    if (cert_der_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_SignCert failed: %d\n", cert_der_len);
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    wc_FreeRsaKey(rsa);
    delete rsa;

    // Convert DER to PEM
    std::vector<uint8_t> cert_pem(BUF_SZ);
    int cert_pem_len = wc_DerToPem(cert_der.data(), cert_der_len,
                                    cert_pem.data(), BUF_SZ, CERT_TYPE);
    if (cert_pem_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_DerToPem (cert) failed: %d\n", cert_pem_len);
        return false;
    }

    std::vector<uint8_t> key_pem(BUF_SZ);
    int key_pem_len = wc_DerToPem(key_der.data(), key_der_len,
                                   key_pem.data(), BUF_SZ, PRIVATEKEY_TYPE);
    if (key_pem_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_DerToPem (key) failed: %d\n", key_pem_len);
        return false;
    }

    // Write PEM files
    std::ofstream cert_out(cert_path, std::ios::binary);
    if (!cert_out) return false;
    cert_out.write(reinterpret_cast<const char*>(cert_pem.data()), cert_pem_len);
    cert_out.close();

    std::ofstream key_out(key_path, std::ios::binary);
    if (!key_out) return false;
    key_out.write(reinterpret_cast<const char*>(key_pem.data()), key_pem_len);
    key_out.close();

    return true;
}

// --- Seed phrase identity ---

std::string generate_seed_phrase() {

    // Pick 12 random words (each word index needs 11 bits, use 2 random bytes per word)
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
    // SHA-256 of seed phrase → 32-byte Ed25519 seed
    uint8_t hash[WC_SHA256_DIGEST_SIZE];
    wc_Sha256Hash(reinterpret_cast<const uint8_t*>(seed_phrase.data()),
                  static_cast<word32>(seed_phrase.size()), hash);
    std::memcpy(sk.data(), hash, 32);

    // Import seed as Ed25519 private key and derive public key
    ed25519_key key;
    if (wc_ed25519_init(&key) != 0) return false;

    int ret = wc_ed25519_import_private_only(sk.data(), ED25519_KEY_SIZE, &key);
    if (ret != 0) {
        std::fprintf(stderr, "[Crypto] ed25519_import_private_only failed: %d\n", ret);
        wc_ed25519_free(&key);
        return false;
    }

    // Make public key from private
    ret = wc_ed25519_make_public(&key, key.p, ED25519_PUB_KEY_SIZE);
    if (ret != 0) {
        std::fprintf(stderr, "[Crypto] ed25519_make_public failed: %d\n", ret);
        wc_ed25519_free(&key);
        return false;
    }
    key.pubKeySet = 1;

    // Export public key
    word32 pub_len = ED25519_PUB_KEY_SIZE;
    ret = wc_ed25519_export_public(&key, pk.data(), &pub_len);
    if (ret != 0) {
        std::fprintf(stderr, "[Crypto] ed25519_export_public failed: %d\n", ret);
        wc_ed25519_free(&key);
        return false;
    }

    wc_ed25519_free(&key);
    return true;
}

bool derive_pubkey(const SecretKey& sk, PublicKey& pk) {
    ed25519_key key;
    if (wc_ed25519_init(&key) != 0) return false;

    int ret = wc_ed25519_import_private_only(sk.data(), ED25519_KEY_SIZE, &key);
    if (ret != 0) {
        wc_ed25519_free(&key);
        return false;
    }

    ret = wc_ed25519_make_public(&key, key.p, ED25519_PUB_KEY_SIZE);
    if (ret != 0) {
        wc_ed25519_free(&key);
        return false;
    }
    key.pubKeySet = 1;

    word32 pub_len = ED25519_PUB_KEY_SIZE;
    ret = wc_ed25519_export_public(&key, pk.data(), &pub_len);
    wc_ed25519_free(&key);
    return ret == 0;
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
    ed25519_key key;
    if (wc_ed25519_init(&key) != 0) return false;

    int ret = wc_ed25519_import_private_key(sk.data(), ED25519_KEY_SIZE,
                                             pk.data(), ED25519_PUB_KEY_SIZE,
                                             &key);
    if (ret != 0) {
        wc_ed25519_free(&key);
        return false;
    }

    word32 sig_len = ED25519_SIG_SIZE;
    ret = wc_ed25519_sign_msg(msg, static_cast<word32>(msg_len),
                               sig_out.data(), &sig_len, &key);
    wc_ed25519_free(&key);
    return ret == 0;
}

bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const Signature& sig, const PublicKey& pk) {
    ed25519_key key;
    if (wc_ed25519_init(&key) != 0) return false;

    int ret = wc_ed25519_import_public(pk.data(), ED25519_PUB_KEY_SIZE, &key);
    if (ret != 0) {
        wc_ed25519_free(&key);
        return false;
    }

    int verified = 0;
    ret = wc_ed25519_verify_msg(sig.data(), ED25519_SIG_SIZE,
                                 msg, static_cast<word32>(msg_len),
                                 &verified, &key);
    wc_ed25519_free(&key);
    return ret == 0 && verified == 1;
}

Fingerprint public_key_fingerprint(const PublicKey& pk) {
    return sha256_hex(pk.data(), pk.size());
}

} // namespace parties
