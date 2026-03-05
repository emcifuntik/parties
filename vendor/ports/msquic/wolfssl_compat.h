/*
 * wolfSSL compatibility shim for MsQuic's tls_quictls.c
 *
 * Provides stubs/replacements for OpenSSL 3.x APIs that wolfSSL doesn't
 * implement. Included at the top of tls_quictls.c when building with wolfSSL.
 */
#pragma once

/* wolfSSL doesn't provide openssl/core_names.h or openssl/kdf.h */
/* Those are only needed by crypt_openssl.c which we don't use */

/* ---- X509 error codes ---- */
#ifndef X509_R_NO_CERT_SET_FOR_US_TO_VERIFY
#define X509_R_NO_CERT_SET_FOR_US_TO_VERIFY 66
#endif

/* ---- SSL options not in wolfSSL ---- */
#ifndef SSL_OP_NO_ANTI_REPLAY
#define SSL_OP_NO_ANTI_REPLAY 0
#endif
#ifndef SSL_OP_ENABLE_MIDDLEBOX_COMPAT
#define SSL_OP_ENABLE_MIDDLEBOX_COMPAT 0
#endif

/* ---- Early Data constants ---- */
/* Provided by wolfSSL when WOLFSSL_EARLY_DATA is defined */

/* ---- Client Hello Callback (not in wolfSSL) ---- */

#ifndef SSL_CLIENT_HELLO_SUCCESS
#define SSL_CLIENT_HELLO_SUCCESS 1
#define SSL_CLIENT_HELLO_ERROR   0

typedef int (*SSL_client_hello_cb_fn)(SSL *ssl, int *al, void *arg);

static inline void SSL_CTX_set_client_hello_cb(
    SSL_CTX *ctx, SSL_client_hello_cb_fn cb, void *arg) {
    (void)ctx; (void)cb; (void)arg;
}

static inline int SSL_client_hello_get0_ext(
    SSL *ssl, unsigned int type,
    const unsigned char **out, size_t *outlen) {
    (void)ssl; (void)type;
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    return 0; /* extension not found */
}
#endif

/* ---- Session Ticket Callbacks (OpenSSL 3.x only) ---- */

#ifndef SSL_TICKET_SUCCESS
typedef int SSL_TICKET_STATUS;
typedef int SSL_TICKET_RETURN;

#define SSL_TICKET_SUCCESS        0
#define SSL_TICKET_SUCCESS_RENEW  1
#define SSL_TICKET_NO_DECRYPT     2
#define SSL_TICKET_FATAL_ERR_MALLOC 3
#define SSL_TICKET_FATAL_ERR_OTHER  4

#define SSL_TICKET_RETURN_ABORT          0
#define SSL_TICKET_RETURN_USE            1
#define SSL_TICKET_RETURN_USE_RENEW      2
#define SSL_TICKET_RETURN_IGNORE         3
#define SSL_TICKET_RETURN_IGNORE_RENEW   4
#endif

/* SSL_CTX_set_session_ticket_cb — pure macro to avoid C4152 (func/data ptr conversion) */
#define SSL_CTX_set_session_ticket_cb(ctx, gen_cb, dec_cb, arg) (1)

/* SSL_SESSION_get0_ticket_appdata / set1 */
static inline int wolfssl_compat_session_get0_ticket_appdata(
    const SSL_SESSION *s, void **data, size_t *len) {
    (void)s;
    if (data) *data = NULL;
    if (len) *len = 0;
    return 0;
}
#define SSL_SESSION_get0_ticket_appdata wolfssl_compat_session_get0_ticket_appdata

static inline int wolfssl_compat_session_set1_ticket_appdata(
    SSL_SESSION *s, const void *data, size_t len) {
    (void)s; (void)data; (void)len;
    return 1;
}
#define SSL_SESSION_set1_ticket_appdata wolfssl_compat_session_set1_ticket_appdata

/* SSL_new_session_ticket */
static inline int wolfssl_compat_new_session_ticket(SSL *ssl) {
    (void)ssl;
    return 1;
}
#define SSL_new_session_ticket wolfssl_compat_new_session_ticket

/* ---- EVP Ticket Key Callback ---- */
#ifdef SSL_CTX_set_tlsext_ticket_key_evp_cb
#undef SSL_CTX_set_tlsext_ticket_key_evp_cb
#endif
#define SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx, cb) ((void)(ctx), (void)(cb))

/* ---- Early Data functions ---- */
/* Provided by wolfSSL natively when WOLFSSL_EARLY_DATA is defined:
   SSL_CTX_set_max_early_data, SSL_set_quic_early_data_enabled,
   SSL_get_early_data_status, SSL_EARLY_DATA_* constants */

/* ---- PKCS7 ---- */
/* wolfSSL's PKCS7 API differs from OpenSSL. The portable cert chain path
   will silently skip (PKCS7_new returns NULL, code checks and handles it). */

#ifndef NID_pkcs7_signed
#define NID_pkcs7_signed 22
#endif
#ifndef NID_pkcs7_data
#define NID_pkcs7_data 21
#endif

/* Forward-declare a dummy PKCS7 type */
typedef struct wolfssl_compat_pkcs7 { int dummy; } PKCS7;

static inline PKCS7 *wolfssl_compat_pkcs7_new(void) { return NULL; }
static inline int wolfssl_compat_pkcs7_set_type(PKCS7 *p, int t) { (void)p; (void)t; return 0; }
static inline int wolfssl_compat_pkcs7_content_new(PKCS7 *p, int t) { (void)p; (void)t; return 0; }
static inline int wolfssl_compat_pkcs7_add_cert(PKCS7 *p, X509 *x) { (void)p; (void)x; return 0; }
static inline int wolfssl_compat_i2d_pkcs7(PKCS7 *p, unsigned char **out) { (void)p; (void)out; return 0; }
static inline void wolfssl_compat_pkcs7_free(PKCS7 *p) { (void)p; }

#ifdef PKCS7_new
#undef PKCS7_new
#endif
#define PKCS7_new         wolfssl_compat_pkcs7_new
#define PKCS7_set_type    wolfssl_compat_pkcs7_set_type
#define PKCS7_content_new wolfssl_compat_pkcs7_content_new
#define PKCS7_add_certificate wolfssl_compat_pkcs7_add_cert
#define i2d_PKCS7         wolfssl_compat_i2d_pkcs7
#define PKCS7_free        wolfssl_compat_pkcs7_free

/* ---- SSL_CIPHER_get_id / SSL_CIPHER_get_protocol_id ---- */
/* wolfSSL's SSL_CIPHER_get_id may return just the 2-byte IANA value (e.g. 0x1301)
   while OpenSSL returns 0x0300XXYY. MsQuic's CxPlatTlsNegotiatedCiphers expects
   the OpenSSL format. Wrap to ensure the 0x0300 prefix is present. */
#ifdef SSL_CIPHER_get_id
#undef SSL_CIPHER_get_id
#endif
static inline unsigned long wolfssl_compat_cipher_get_id(const SSL_CIPHER *c) {
    unsigned long id = wolfSSL_CIPHER_get_id(c);
    if ((id & 0xFFFF0000UL) == 0) {
        id |= 0x03000000UL;
    }
    return id;
}
#define SSL_CIPHER_get_id wolfssl_compat_cipher_get_id

static inline unsigned short wolfssl_compat_cipher_get_protocol_id(const SSL_CIPHER *c) {
    return (unsigned short)(wolfSSL_CIPHER_get_id(c) & 0xFFFF);
}
#define SSL_CIPHER_get_protocol_id wolfssl_compat_cipher_get_protocol_id

/* ---- ERR_get_error_all (OpenSSL 3.x) ---- */
static inline unsigned long wolfssl_compat_err_get_error_all(
    const char **file, int *line,
    const char **func, const char **data, int *flags) {
    if (file) *file = "unknown";
    if (line) *line = 0;
    if (func) *func = NULL;
    if (data) *data = NULL;
    if (flags) *flags = 0;
    return ERR_get_error();
}
#define ERR_get_error_all wolfssl_compat_err_get_error_all

/* ---- Session PEM Serialization ---- */
/* Replace with DER serialization using i2d_SSL_SESSION / d2i_SSL_SESSION. */

static inline int wolfssl_compat_write_session_to_bio(BIO *bio, SSL_SESSION *sess) {
    int len = i2d_SSL_SESSION(sess, NULL);
    if (len <= 0) return 0;
    unsigned char *buf = (unsigned char *)OPENSSL_malloc(len);
    if (!buf) return 0;
    unsigned char *p = buf;
    len = i2d_SSL_SESSION(sess, &p);
    if (len > 0) {
        BIO_write(bio, buf, len);
    }
    OPENSSL_free(buf);
    return (len > 0) ? 1 : 0;
}
#define PEM_write_bio_SSL_SESSION wolfssl_compat_write_session_to_bio

static inline SSL_SESSION *wolfssl_compat_read_session_from_bio(
    BIO *bio, SSL_SESSION **x, void *cb, void *u) {
    (void)x; (void)cb; (void)u;
    unsigned char *data = NULL;
    long len = BIO_get_mem_data(bio, &data);
    if (!data || len <= 0) return NULL;
    const unsigned char *p = data;
    return d2i_SSL_SESSION(NULL, &p, len);
}
#define PEM_read_bio_SSL_SESSION wolfssl_compat_read_session_from_bio

/* ======================================================================
 * Minimal stubs for OpenSSL 3.x types still referenced by tls_quictls.c
 *
 * The session ticket key callback (CxPlatTlsOnSessionTicketKeyNeeded) uses
 * OSSL_PARAM and EVP_MAC_CTX in its signature, but the callback is never
 * registered (SSL_CTX_set_tlsext_ticket_key_evp_cb is a no-op above).
 * These stubs only need to compile, not execute.
 * ====================================================================== */

#include <string.h> /* strlen */

/* ---- OSSL_PARAM (compile-only, for ticket key callback signature) ---- */

#define WOLFSSL_OSSL_PARAM_END          0
#define WOLFSSL_OSSL_PARAM_UTF8_STRING  1
#define WOLFSSL_OSSL_PARAM_OCTET_STRING 2

#ifndef OSSL_PARAM
typedef struct {
    const char *key;
    int         data_type;
    void       *data;
    size_t      data_size;
} OSSL_PARAM;

static inline OSSL_PARAM
OSSL_PARAM_construct_utf8_string(const char *key, char *val, size_t len) {
    OSSL_PARAM p;
    p.key = key;
    p.data_type = WOLFSSL_OSSL_PARAM_UTF8_STRING;
    p.data = val;
    p.data_size = (len != 0) ? len : (val ? strlen(val) : 0);
    return p;
}

static inline OSSL_PARAM
OSSL_PARAM_construct_octet_string(const char *key, void *val, size_t len) {
    OSSL_PARAM p;
    p.key = key;
    p.data_type = WOLFSSL_OSSL_PARAM_OCTET_STRING;
    p.data = val;
    p.data_size = len;
    return p;
}

static inline OSSL_PARAM
OSSL_PARAM_construct_end(void) {
    OSSL_PARAM p;
    p.key = NULL;
    p.data_type = WOLFSSL_OSSL_PARAM_END;
    p.data = NULL;
    p.data_size = 0;
    return p;
}

#define OSSL_MAC_PARAM_KEY    "key"
#define OSSL_MAC_PARAM_DIGEST "digest"
#endif /* OSSL_PARAM */

/* ---- EVP_MAC_CTX (compile-only stub) ---- */
/* Only used as a parameter type in the disabled ticket key callback. */

typedef struct { int dummy; } wolfssl_compat_evp_mac_ctx_t;
#define EVP_MAC_CTX wolfssl_compat_evp_mac_ctx_t

static inline int
wolfssl_compat_mac_ctx_set_params(EVP_MAC_CTX *ctx, const OSSL_PARAM *p) {
    (void)ctx; (void)p;
    return 1;
}
#define EVP_MAC_CTX_set_params wolfssl_compat_mac_ctx_set_params
