#pragma once

// Enable MsQuic insecure features API (QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION).
// The MsQuic core library already compiles this in; the define only unhides
// the constant in the public header.
#define QUIC_API_ENABLE_INSECURE_FEATURES 1
#include <msquic.h>

namespace parties {

// Initialize the MsQuic library (call once at startup).
// Returns the API table, or nullptr on failure.
const QUIC_API_TABLE* quic_init();

// Clean up MsQuic (call once at shutdown).
void quic_cleanup();

// Get the API table (must call quic_init() first).
const QUIC_API_TABLE* quic_api();

// Our ALPN identifier
inline QUIC_BUFFER make_alpn() {
    static uint8_t alpn[] = "parties";
    return { 7, alpn };
}

} // namespace parties
