#include <parties/quic_common.h>
#include <cstdio>

namespace parties {

static const QUIC_API_TABLE* g_quic_api = nullptr;

const QUIC_API_TABLE* quic_init() {
    if (g_quic_api) return g_quic_api;

    QUIC_STATUS status = MsQuicOpen2(&g_quic_api);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "[QUIC] MsQuicOpen2 failed: 0x%lx\n", (unsigned long)status);
        return nullptr;
    }

    std::printf("[QUIC] MsQuic initialized\n");
    return g_quic_api;
}

void quic_cleanup() {
    if (g_quic_api) {
        MsQuicClose(g_quic_api);
        g_quic_api = nullptr;
        std::printf("[QUIC] MsQuic cleaned up\n");
    }
}

const QUIC_API_TABLE* quic_api() {
    return g_quic_api;
}

} // namespace parties
