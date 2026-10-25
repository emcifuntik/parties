#pragma once

#include <msquic.h>

namespace parties {

const QUIC_API_TABLE* quic_init();

void quic_cleanup();

const QUIC_API_TABLE* quic_api();

inline QUIC_BUFFER make_alpn() {
    static uint8_t alpn[] = "parties";
    return { 7, alpn };
}

}
