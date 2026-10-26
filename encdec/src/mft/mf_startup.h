#pragma once

#include <mfapi.h>

#include <optional>
#include <utility>

#include <parties/log.h>

namespace parties::encdec::mft {

enum class MfStartupMode : DWORD {
    Full = MFSTARTUP_FULL,
    Lite = MFSTARTUP_LITE,
};

class MfStartupGuard {
public:
    static std::optional<MfStartupGuard> start(MfStartupMode mode) {
        const HRESULT hr = MFStartup(MF_VERSION, static_cast<DWORD>(mode));
        if (FAILED(hr)) {
            LOG_ERROR("MFStartup failed: {:#010x}", hr);
            return std::nullopt;
        }
        return MfStartupGuard{};
    }

    MfStartupGuard(MfStartupGuard&& other) noexcept
        : owns_startup_(std::exchange(other.owns_startup_, false)) {}
    MfStartupGuard(const MfStartupGuard&) = delete;
    MfStartupGuard& operator=(const MfStartupGuard&) = delete;
    MfStartupGuard& operator=(MfStartupGuard&& other) noexcept {
        if (this != &other) {
            release();
            owns_startup_ = std::exchange(other.owns_startup_, false);
        }
        return *this;
    }

    ~MfStartupGuard() { release(); }

private:
    MfStartupGuard() = default;

    void release() {
        if (std::exchange(owns_startup_, false)) MFShutdown();
    }

    bool owns_startup_ = true;
};

}
