#include <parties/quic_common.h>
#include <parties/log.h>

namespace parties {

static const QUIC_API_TABLE* g_quic_api = nullptr;

const QUIC_API_TABLE* quic_init() {
    if (g_quic_api) return g_quic_api;

    QUIC_STATUS status = MsQuicOpen2(&g_quic_api);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("MsQuicOpen2 failed: {:#x}", (unsigned long)status);
        return nullptr;
    }

    LOG_INFO("MsQuic initialized");
    return g_quic_api;
}

void quic_cleanup() {
    if (g_quic_api) {
        MsQuicClose(g_quic_api);
        g_quic_api = nullptr;
        LOG_INFO("MsQuic cleaned up");
    }
}

const QUIC_API_TABLE* quic_api() {
    return g_quic_api;
}

}
