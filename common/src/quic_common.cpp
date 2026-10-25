#include <parties/quic_common.h>
#include <parties/log.h>

#include <utility>

namespace parties {

static const QUIC_API_TABLE* g_quic_api = nullptr;

std::optional<QuicLibrary> QuicLibrary::open() {
    if (g_quic_api) {
        LOG_ERROR("MsQuic is already open in this process");
        return std::nullopt;
    }
    const QUIC_API_TABLE* api = nullptr;
    const QUIC_STATUS status = MsQuicOpen2(&api);
    if (QUIC_FAILED(status)) {
        LOG_ERROR("MsQuicOpen2 failed: {:#x}", (unsigned long)status);
        return std::nullopt;
    }
    g_quic_api = api;
    LOG_INFO("MsQuic initialized");
    return QuicLibrary(api);
}

QuicLibrary::QuicLibrary(QuicLibrary&& other) noexcept
    : api_(std::exchange(other.api_, nullptr)) {}

QuicLibrary::~QuicLibrary() {
    if (!api_) return;
    MsQuicClose(api_);
    g_quic_api = nullptr;
    LOG_INFO("MsQuic cleaned up");
}

const QUIC_API_TABLE* quic_api() {
    return g_quic_api;
}

}
