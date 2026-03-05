#include <parties/crypto.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/asn.h>

#include <cstdio>
#include <fstream>
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

} // namespace parties
