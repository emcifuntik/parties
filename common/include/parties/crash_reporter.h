#pragma once

#include <optional>
#include <string_view>

namespace parties {

class LogSession;

class CrashReporterSession {
public:
    CrashReporterSession(const LogSession& log, std::optional<std::string_view> dsn);
    ~CrashReporterSession();
    CrashReporterSession(const CrashReporterSession&) = delete;
    CrashReporterSession& operator=(const CrashReporterSession&) = delete;

private:
    bool active_ = false;
};

}
