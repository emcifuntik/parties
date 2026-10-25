#include <parties/crash_reporter.h>
#include <parties/log.h>
#include <parties/version.h>

#ifdef SENTRY_ENABLED
#include <sentry.h>
#endif

#include <string>

namespace parties {

CrashReporterSession::CrashReporterSession([[maybe_unused]] const LogSession& log,
                                           [[maybe_unused]] std::optional<std::string_view> dsn) {
#ifdef SENTRY_ENABLED
    if (!dsn || dsn->empty()) return;
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, std::string(*dsn).c_str());
    sentry_options_set_release(options, APP_VERSION);
#ifdef PARTIES_RETAIL
    sentry_options_set_environment(options, "production");
#else
    sentry_options_set_environment(options, "development");
#endif
    sentry_options_set_database_path(options, ".sentry-native");
    sentry_options_set_auto_session_tracking(options, 1);
    if (log.file_path())
        sentry_options_add_attachment(options, log.file_path()->string().c_str());
    active_ = sentry_init(options) == 0;
    if (active_) sentry_start_session();
#endif
}

CrashReporterSession::~CrashReporterSession() {
#ifdef SENTRY_ENABLED
    if (!active_) return;
    sentry_end_session();
    sentry_close();
#endif
}

}
