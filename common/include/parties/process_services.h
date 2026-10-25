#pragma once

#include <parties/crash_reporter.h>
#include <parties/log.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>

#include <memory>
#include <optional>
#include <string_view>

namespace parties {

class ProcessServices {
public:
    struct Options {
        LogTarget log_target;
        std::optional<std::string_view> sentry_dsn;
    };

    static std::unique_ptr<ProcessServices> start(const Options& options);

    ProcessServices(const ProcessServices&) = delete;
    ProcessServices& operator=(const ProcessServices&) = delete;

    const LogSession& log() const { return log_; }

private:
    explicit ProcessServices(const Options& options);

    LogSession                 log_;
    CrashReporterSession       crash_;
    std::optional<NetLibrary>  net_;
    std::optional<QuicLibrary> quic_;
};

}
