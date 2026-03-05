/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Implements the cryptographic functions using wolfSSL's native wolfcrypt API.
    Replaces both crypt_openssl.c (Linux) and crypt_bcrypt.c (Windows).

--*/

#include "platform_internal.h"

#ifdef QUIC_CLOG
#include "crypt_wolfssl.c.clog.h"
#endif

/* wolfSSL native crypto headers */
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/chacha.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

/* wolfSSL EVP compat — only for CXPLAT_AES_256_CBC_ALG_HANDLE (used by tls_quictls.c) */
#include "openssl/evp.h"

/*
 * Global cipher handle consumed by tls_quictls.c for session ticket
 * encrypt/decrypt (AES-256-CBC via wolfSSL's EVP compat layer).
 */
EVP_CIPHER *CXPLAT_AES_256_CBC_ALG_HANDLE;

/* ---- Initialization / Cleanup ---- */

QUIC_STATUS
CxPlatCryptInitialize(
    void
    )
{
    CXPLAT_AES_256_CBC_ALG_HANDLE = (EVP_CIPHER *)EVP_aes_256_cbc();
    if (CXPLAT_AES_256_CBC_ALG_HANDLE == NULL) {
        QuicTraceEvent(
            LibraryError,
            "[ lib] ERROR, %s.",
            "EVP_aes_256_cbc returned NULL");
        return QUIC_STATUS_TLS_ERROR;
    }
    return QUIC_STATUS_SUCCESS;
}

void
CxPlatCryptUninitialize(
    void
    )
{
    CXPLAT_AES_256_CBC_ALG_HANDLE = NULL;
}

BOOLEAN
CxPlatCryptSupports(
    CXPLAT_AEAD_TYPE AeadType
    )
{
    switch (AeadType) {
    case CXPLAT_AEAD_AES_128_GCM:
    case CXPLAT_AEAD_AES_256_GCM:
        return TRUE;
    case CXPLAT_AEAD_CHACHA20_POLY1305:
#ifdef HAVE_CHACHA
        return TRUE;
#else
        return FALSE;
#endif
    default:
        return FALSE;
    }
}

/* ---- AEAD Key (packet encryption) ---- */

typedef struct CXPLAT_KEY {
    CXPLAT_AEAD_TYPE Aead;
    union {
        Aes     AesCtx;
        uint8_t ChaChaKey[CHACHA20_POLY1305_AEAD_KEYSIZE];
    };
} CXPLAT_KEY;

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatKeyCreate(
    _In_ CXPLAT_AEAD_TYPE AeadType,
    _When_(AeadType == CXPLAT_AEAD_AES_128_GCM, _In_reads_(16))
    _When_(AeadType == CXPLAT_AEAD_AES_256_GCM, _In_reads_(32))
    _When_(AeadType == CXPLAT_AEAD_CHACHA20_POLY1305, _In_reads_(32))
        const uint8_t* const RawKey,
    _Out_ CXPLAT_KEY** NewKey
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    int Ret;

    CXPLAT_KEY* Key = CXPLAT_ALLOC_NONPAGED(sizeof(CXPLAT_KEY), QUIC_POOL_TLS_KEY);
    if (Key == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_KEY",
            sizeof(CXPLAT_KEY));
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    CxPlatZeroMemory(Key, sizeof(CXPLAT_KEY));
    Key->Aead = AeadType;

    switch (AeadType) {
    case CXPLAT_AEAD_AES_128_GCM:
        Ret = wc_AesInit(&Key->AesCtx, NULL, INVALID_DEVID);
        if (Ret != 0) {
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        Ret = wc_AesGcmSetKey(&Key->AesCtx, RawKey, 16);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesGcmSetKey (128) failed");
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        break;

    case CXPLAT_AEAD_AES_256_GCM:
        Ret = wc_AesInit(&Key->AesCtx, NULL, INVALID_DEVID);
        if (Ret != 0) {
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        Ret = wc_AesGcmSetKey(&Key->AesCtx, RawKey, 32);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesGcmSetKey (256) failed");
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        break;

    case CXPLAT_AEAD_CHACHA20_POLY1305:
#ifdef HAVE_CHACHA
        CxPlatCopyMemory(Key->ChaChaKey, RawKey, CHACHA20_POLY1305_AEAD_KEYSIZE);
        break;
#else
        Status = QUIC_STATUS_NOT_SUPPORTED;
        goto Exit;
#endif

    default:
        Status = QUIC_STATUS_NOT_SUPPORTED;
        goto Exit;
    }

    *NewKey = Key;
    Key = NULL;

Exit:
    if (Key != NULL) {
        CXPLAT_FREE(Key, QUIC_POOL_TLS_KEY);
    }
    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatKeyFree(
    _In_opt_ CXPLAT_KEY* Key
    )
{
    if (Key != NULL) {
        if (Key->Aead == CXPLAT_AEAD_AES_128_GCM ||
            Key->Aead == CXPLAT_AEAD_AES_256_GCM) {
            wc_AesFree(&Key->AesCtx);
        }
        CXPLAT_FREE(Key, QUIC_POOL_TLS_KEY);
    }
}

/* ---- AEAD Encrypt / Decrypt ---- */

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatEncrypt(
    _In_ CXPLAT_KEY* Key,
    _In_reads_bytes_(CXPLAT_IV_LENGTH)
        const uint8_t* const Iv,
    _In_ uint16_t AuthDataLength,
    _In_reads_bytes_opt_(AuthDataLength)
        const uint8_t* const AuthData,
    _In_ uint16_t BufferLength,
    _When_(BufferLength > CXPLAT_ENCRYPTION_OVERHEAD, _Inout_updates_bytes_(BufferLength))
    _When_(BufferLength <= CXPLAT_ENCRYPTION_OVERHEAD, _Out_writes_bytes_(BufferLength))
        uint8_t* Buffer
    )
{
    CXPLAT_DBG_ASSERT(CXPLAT_ENCRYPTION_OVERHEAD <= BufferLength);

    const uint16_t PlainTextLength = BufferLength - CXPLAT_ENCRYPTION_OVERHEAD;
    uint8_t* Tag = Buffer + PlainTextLength;
    int Ret;

    switch (Key->Aead) {
    case CXPLAT_AEAD_AES_128_GCM:
    case CXPLAT_AEAD_AES_256_GCM:
        Ret = wc_AesGcmEncrypt(
            &Key->AesCtx,
            Buffer,                     /* out (ciphertext, in-place) */
            Buffer,                     /* in (plaintext) */
            PlainTextLength,
            Iv,
            CXPLAT_IV_LENGTH,
            Tag,                        /* auth tag output */
            CXPLAT_ENCRYPTION_OVERHEAD,
            AuthData,
            AuthDataLength);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesGcmEncrypt failed");
            return QUIC_STATUS_TLS_ERROR;
        }
        break;

#ifdef HAVE_CHACHA
    case CXPLAT_AEAD_CHACHA20_POLY1305:
        Ret = wc_ChaCha20Poly1305_Encrypt(
            Key->ChaChaKey,
            Iv,
            AuthData,
            AuthDataLength,
            Buffer,                     /* in (plaintext) */
            PlainTextLength,
            Buffer,                     /* out (ciphertext, in-place) */
            Tag);                       /* auth tag output */
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_ChaCha20Poly1305_Encrypt failed");
            return QUIC_STATUS_TLS_ERROR;
        }
        break;
#endif

    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }

    return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatDecrypt(
    _In_ CXPLAT_KEY* Key,
    _In_reads_bytes_(CXPLAT_IV_LENGTH)
        const uint8_t* const Iv,
    _In_ uint16_t AuthDataLength,
    _In_reads_bytes_opt_(AuthDataLength)
        const uint8_t* const AuthData,
    _In_ uint16_t BufferLength,
    _Inout_updates_bytes_(BufferLength)
        uint8_t* Buffer
    )
{
    CXPLAT_DBG_ASSERT(CXPLAT_ENCRYPTION_OVERHEAD <= BufferLength);

    const uint16_t CipherTextLength = BufferLength - CXPLAT_ENCRYPTION_OVERHEAD;
    const uint8_t* Tag = Buffer + CipherTextLength;
    int Ret;

    switch (Key->Aead) {
    case CXPLAT_AEAD_AES_128_GCM:
    case CXPLAT_AEAD_AES_256_GCM:
        Ret = wc_AesGcmDecrypt(
            &Key->AesCtx,
            Buffer,                     /* out (plaintext, in-place) */
            Buffer,                     /* in (ciphertext) */
            CipherTextLength,
            Iv,
            CXPLAT_IV_LENGTH,
            Tag,                        /* auth tag to verify */
            CXPLAT_ENCRYPTION_OVERHEAD,
            AuthData,
            AuthDataLength);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesGcmDecrypt failed");
            return QUIC_STATUS_TLS_ERROR;
        }
        break;

#ifdef HAVE_CHACHA
    case CXPLAT_AEAD_CHACHA20_POLY1305:
        Ret = wc_ChaCha20Poly1305_Decrypt(
            Key->ChaChaKey,
            Iv,
            AuthData,
            AuthDataLength,
            Buffer,                     /* in (ciphertext) */
            CipherTextLength,
            Tag,                        /* auth tag to verify */
            Buffer);                    /* out (plaintext, in-place) */
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_ChaCha20Poly1305_Decrypt failed");
            return QUIC_STATUS_TLS_ERROR;
        }
        break;
#endif

    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }

    return QUIC_STATUS_SUCCESS;
}

/* ---- Header Protection ---- */

typedef struct CXPLAT_HP_KEY {
    CXPLAT_AEAD_TYPE Aead;
    union {
        Aes    AesCtx;      /* AES-128/256-ECB */
        ChaCha ChaChaCtx;   /* ChaCha20 stream cipher */
    };
} CXPLAT_HP_KEY;

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatHpKeyCreate(
    _In_ CXPLAT_AEAD_TYPE AeadType,
    _When_(AeadType == CXPLAT_AEAD_AES_128_GCM, _In_reads_(16))
    _When_(AeadType == CXPLAT_AEAD_AES_256_GCM, _In_reads_(32))
    _When_(AeadType == CXPLAT_AEAD_CHACHA20_POLY1305, _In_reads_(32))
        const uint8_t* const RawKey,
    _Out_ CXPLAT_HP_KEY** NewKey
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    int Ret;

    CXPLAT_HP_KEY* Key = CXPLAT_ALLOC_NONPAGED(sizeof(CXPLAT_HP_KEY), QUIC_POOL_TLS_HP_KEY);
    if (Key == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_HP_KEY",
            sizeof(CXPLAT_HP_KEY));
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    CxPlatZeroMemory(Key, sizeof(CXPLAT_HP_KEY));
    Key->Aead = AeadType;

    switch (AeadType) {
    case CXPLAT_AEAD_AES_128_GCM:
        Ret = wc_AesInit(&Key->AesCtx, NULL, INVALID_DEVID);
        if (Ret != 0) {
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        Ret = wc_AesSetKeyDirect(&Key->AesCtx, RawKey, 16, NULL, AES_ENCRYPTION);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesSetKeyDirect (128) failed");
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        break;

    case CXPLAT_AEAD_AES_256_GCM:
        Ret = wc_AesInit(&Key->AesCtx, NULL, INVALID_DEVID);
        if (Ret != 0) {
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        Ret = wc_AesSetKeyDirect(&Key->AesCtx, RawKey, 32, NULL, AES_ENCRYPTION);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_AesSetKeyDirect (256) failed");
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        break;

    case CXPLAT_AEAD_CHACHA20_POLY1305:
#ifdef HAVE_CHACHA
        Ret = wc_Chacha_SetKey(&Key->ChaChaCtx, RawKey, CHACHA20_POLY1305_AEAD_KEYSIZE);
        if (Ret != 0) {
            QuicTraceEvent(
                LibraryErrorStatus,
                "[ lib] ERROR, %u, %s.",
                Ret,
                "wc_Chacha_SetKey failed");
            Status = QUIC_STATUS_TLS_ERROR;
            goto Exit;
        }
        break;
#else
        Status = QUIC_STATUS_NOT_SUPPORTED;
        goto Exit;
#endif

    default:
        Status = QUIC_STATUS_NOT_SUPPORTED;
        goto Exit;
    }

    *NewKey = Key;
    Key = NULL;

Exit:
    CxPlatHpKeyFree(Key);
    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatHpKeyFree(
    _In_opt_ CXPLAT_HP_KEY* Key
    )
{
    if (Key != NULL) {
        if (Key->Aead == CXPLAT_AEAD_AES_128_GCM ||
            Key->Aead == CXPLAT_AEAD_AES_256_GCM) {
            wc_AesFree(&Key->AesCtx);
        }
        CXPLAT_FREE(Key, QUIC_POOL_TLS_HP_KEY);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatHpComputeMask(
    _In_ CXPLAT_HP_KEY* Key,
    _In_ uint8_t BatchSize,
    _In_reads_bytes_(CXPLAT_HP_SAMPLE_LENGTH * BatchSize)
        const uint8_t* const Cipher,
    _Out_writes_bytes_(CXPLAT_HP_SAMPLE_LENGTH * BatchSize)
        uint8_t* Mask
    )
{
    int Ret;

    if (Key->Aead == CXPLAT_AEAD_CHACHA20_POLY1305) {
#ifdef HAVE_CHACHA
        /*
         * RFC 9001 Section 5.4.4: ChaCha20 header protection.
         * Sample = counter(4 bytes LE) || nonce(12 bytes).
         * Encrypt 5 zero bytes to produce the mask.
         */
        static const uint8_t Zero[5] = { 0 };
        uint32_t i, Offset;
        for (i = 0, Offset = 0; i < BatchSize; ++i, Offset += CXPLAT_HP_SAMPLE_LENGTH) {
            uint32_t Counter;
            CxPlatCopyMemory(&Counter, Cipher + Offset, sizeof(Counter));

            Ret = wc_Chacha_SetIV(&Key->ChaChaCtx, Cipher + Offset + 4, Counter);
            if (Ret != 0) {
                QuicTraceEvent(
                    LibraryError,
                    "[ lib] ERROR, %s.",
                    "wc_Chacha_SetIV (hp) failed");
                return QUIC_STATUS_TLS_ERROR;
            }
            Ret = wc_Chacha_Process(&Key->ChaChaCtx, Mask + Offset, Zero, sizeof(Zero));
            if (Ret != 0) {
                QuicTraceEvent(
                    LibraryError,
                    "[ lib] ERROR, %s.",
                    "wc_Chacha_Process (hp) failed");
                return QUIC_STATUS_TLS_ERROR;
            }
        }
#else
        return QUIC_STATUS_NOT_SUPPORTED;
#endif
    } else {
        /*
         * AES-ECB header protection: encrypt each 16-byte sample block.
         * wc_AesEncryptDirect processes one AES block (16 bytes) at a time.
         */
        uint32_t i;
        for (i = 0; i < BatchSize; ++i) {
            Ret = wc_AesEncryptDirect(
                &Key->AesCtx,
                Mask + i * CXPLAT_HP_SAMPLE_LENGTH,
                Cipher + i * CXPLAT_HP_SAMPLE_LENGTH);
            if (Ret != 0) {
                QuicTraceEvent(
                    LibraryError,
                    "[ lib] ERROR, %s.",
                    "wc_AesEncryptDirect (hp) failed");
                return QUIC_STATUS_TLS_ERROR;
            }
        }
    }

    return QUIC_STATUS_SUCCESS;
}

/* ---- HMAC Hash ---- */

typedef struct CXPLAT_HASH {
    Hmac     HmacCtx;
    int      WcHashType;
    uint32_t SaltLength;
    uint8_t  Salt[0];
} CXPLAT_HASH;

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatHashCreate(
    _In_ CXPLAT_HASH_TYPE HashType,
    _In_reads_(SaltLength)
        const uint8_t* const Salt,
    _In_ uint32_t SaltLength,
    _Out_ CXPLAT_HASH** NewHash
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    int WcType;
    int Ret;

    switch (HashType) {
    case CXPLAT_HASH_SHA256: WcType = WC_SHA256; break;
    case CXPLAT_HASH_SHA384: WcType = WC_SHA384; break;
    case CXPLAT_HASH_SHA512: WcType = WC_SHA512; break;
    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }

    CXPLAT_HASH* Hash =
        CXPLAT_ALLOC_NONPAGED(sizeof(CXPLAT_HASH) + SaltLength, QUIC_POOL_TLS_HASH);
    if (Hash == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_HASH",
            sizeof(CXPLAT_HASH) + SaltLength);
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    CxPlatZeroMemory(Hash, sizeof(CXPLAT_HASH));

    Hash->WcHashType = WcType;
    Hash->SaltLength = SaltLength;
    CxPlatCopyMemory(Hash->Salt, Salt, SaltLength);

    Ret = wc_HmacInit(&Hash->HmacCtx, NULL, INVALID_DEVID);
    if (Ret != 0) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Ret,
            "wc_HmacInit failed");
        Status = QUIC_STATUS_TLS_ERROR;
        goto Exit;
    }

    *NewHash = Hash;
    Hash = NULL;

Exit:
    if (Hash != NULL) {
        CXPLAT_FREE(Hash, QUIC_POOL_TLS_HASH);
    }
    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatHashFree(
    _In_opt_ CXPLAT_HASH* Hash
    )
{
    if (Hash) {
        wc_HmacFree(&Hash->HmacCtx);
        CXPLAT_FREE(Hash, QUIC_POOL_TLS_HASH);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatHashCompute(
    _In_ CXPLAT_HASH* Hash,
    _In_reads_(InputLength)
        const uint8_t* const Input,
    _In_ uint32_t InputLength,
    _In_ uint32_t OutputLength,
    _Out_writes_all_(OutputLength)
        uint8_t* const Output
    )
{
    int Ret;

    Ret = wc_HmacSetKey(&Hash->HmacCtx, Hash->WcHashType,
                         Hash->Salt, Hash->SaltLength);
    if (Ret != 0) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Ret,
            "wc_HmacSetKey failed");
        return QUIC_STATUS_INTERNAL_ERROR;
    }

    Ret = wc_HmacUpdate(&Hash->HmacCtx, Input, InputLength);
    if (Ret != 0) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Ret,
            "wc_HmacUpdate failed");
        return QUIC_STATUS_INTERNAL_ERROR;
    }

    /*
     * wc_HmacFinal outputs the full digest. We write into a stack buffer
     * if the caller wants a truncated output (shouldn't happen for QUIC,
     * but be safe).
     */
    uint8_t FullDigest[CXPLAT_HASH_MAX_SIZE];
    Ret = wc_HmacFinal(&Hash->HmacCtx, FullDigest);
    if (Ret != 0) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Ret,
            "wc_HmacFinal failed");
        return QUIC_STATUS_INTERNAL_ERROR;
    }

    CxPlatCopyMemory(Output, FullDigest, OutputLength);
    return QUIC_STATUS_SUCCESS;
}

/* ---- KBKDF (SP 800-108 counter mode with HMAC-SHA256) ---- */

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatKbKdfDerive(
    _In_reads_(SecretLength) const uint8_t* Secret,
    _In_ uint32_t SecretLength,
    _In_z_ const char* Label,
    _In_reads_opt_(ContextLength) const uint8_t* Context,
    _In_ uint32_t ContextLength,
    _In_ uint32_t OutputLength,
    _Out_writes_(OutputLength) uint8_t* Output
    )
{
    /*
     * SP 800-108 KBKDF in counter mode:
     *   K(i) = HMAC-SHA256(Secret, [i]_32BE || Label || 0x00 || Context || [L]_32BE)
     * where i starts at 1, L = output length in bits.
     */
    uint32_t Done = 0;
    uint32_t Counter = 1;
    size_t LabelLength = strnlen(Label, 255);
    uint32_t L_bits = OutputLength * 8;
    uint8_t L_buf[4];
    uint8_t Separator = 0x00;
    Hmac Hmac;
    int Ret;

    L_buf[0] = (uint8_t)((L_bits >> 24) & 0xFF);
    L_buf[1] = (uint8_t)((L_bits >> 16) & 0xFF);
    L_buf[2] = (uint8_t)((L_bits >>  8) & 0xFF);
    L_buf[3] = (uint8_t)( L_bits        & 0xFF);

    Ret = wc_HmacInit(&Hmac, NULL, INVALID_DEVID);
    if (Ret != 0) {
        return QUIC_STATUS_INTERNAL_ERROR;
    }

    while (Done < OutputLength) {
        uint8_t HmacOut[WC_SHA256_DIGEST_SIZE];
        uint8_t CounterBuf[4];
        uint32_t ToCopy;

        CounterBuf[0] = (uint8_t)((Counter >> 24) & 0xFF);
        CounterBuf[1] = (uint8_t)((Counter >> 16) & 0xFF);
        CounterBuf[2] = (uint8_t)((Counter >>  8) & 0xFF);
        CounterBuf[3] = (uint8_t)( Counter        & 0xFF);

        if (wc_HmacSetKey(&Hmac, WC_SHA256, Secret, SecretLength) != 0 ||
            wc_HmacUpdate(&Hmac, CounterBuf, 4) != 0 ||
            (LabelLength > 0 &&
             wc_HmacUpdate(&Hmac, (const uint8_t*)Label, (uint32_t)LabelLength) != 0) ||
            wc_HmacUpdate(&Hmac, &Separator, 1) != 0 ||
            (ContextLength > 0 && Context != NULL &&
             wc_HmacUpdate(&Hmac, Context, ContextLength) != 0) ||
            wc_HmacUpdate(&Hmac, L_buf, 4) != 0 ||
            wc_HmacFinal(&Hmac, HmacOut) != 0) {

            QuicTraceEvent(
                LibraryError,
                "[ lib] ERROR, %s.",
                "KBKDF HMAC iteration failed");
            wc_HmacFree(&Hmac);
            return QUIC_STATUS_INTERNAL_ERROR;
        }

        ToCopy = OutputLength - Done;
        if (ToCopy > WC_SHA256_DIGEST_SIZE) {
            ToCopy = WC_SHA256_DIGEST_SIZE;
        }
        CxPlatCopyMemory(Output + Done, HmacOut, ToCopy);
        Done += ToCopy;
        Counter++;
    }

    wc_HmacFree(&Hmac);
    return QUIC_STATUS_SUCCESS;
}
