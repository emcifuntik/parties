#include <parties/quic_common.h>
#include <parties/log.h>

#include <cstdlib>
#include <cstring>

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

bool quic_disable_datapath_offloads(bool disable) {
    if (!g_quic_api) return false;
    uint32_t flags = disable
        ? (QUIC_DATAPATH_OFFLOAD_SEND_SEGMENTATION | QUIC_DATAPATH_OFFLOAD_RECV_COALESCING)
        : 0u;
    const QUIC_STATUS status = g_quic_api->SetParam(
        nullptr, QUIC_PARAM_GLOBAL_DATAPATH_DISABLE_OFFLOADS, sizeof(flags), &flags);
    if (QUIC_FAILED(status)) {
        LOG_WARN("Datapath offloads {}: too late for this run ({:#x}), applies after a restart",
                 disable ? "off" : "on", (unsigned long)status);
        return false;
    }
    LOG_INFO("Datapath UDP offloads (USO/URO) {}", disable ? "disabled" : "allowed");
    return true;
}

// QUIC_CONGESTION_CONTROL_ALGORITHM_BBR. msquic.h only names it with
// QUIC_API_ENABLE_PREVIEW_FEATURES, which would also change other public
// structs; the library itself always implements it.
static constexpr uint16_t QUIC_CC_BBR = 1;
static_assert(QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC == 0, "MsQuic congestion control ids changed");

void apply_media_transport_settings(QUIC_SETTINGS& settings) {
    // PARTIES_QUIC_CC=cubic switches back to CUBIC (field diagnostics and A/B tests).
    static const bool cubic = [] {
        char* value = nullptr;
        size_t len = 0;
#ifdef _WIN32
        if (_dupenv_s(&value, &len, "PARTIES_QUIC_CC") != 0) value = nullptr;
        const bool is_cubic = value && std::strcmp(value, "cubic") == 0;
        std::free(value);
#else
        (void)len;
        value = std::getenv("PARTIES_QUIC_CC");
        const bool is_cubic = value && std::strcmp(value, "cubic") == 0;
#endif
        if (is_cubic) LOG_WARN("PARTIES_QUIC_CC=cubic: using CUBIC congestion control");
        return is_cubic;
    }();
    settings.CongestionControlAlgorithm = cubic ? QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC : QUIC_CC_BBR;
    settings.IsSet.CongestionControlAlgorithm = TRUE;
    settings.PeerUnidiStreamCount = 1024;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
}

void apply_compatibility_transport_settings(QUIC_SETTINGS& settings) {
    settings.MinimumMtu = 1280;
    settings.IsSet.MinimumMtu = TRUE;
    settings.MaximumMtu = 1280;
    settings.IsSet.MaximumMtu = TRUE;
}

} // namespace parties
