#include <parties/crash_reporter.h>
#include <parties/version.h>

#ifdef SENTRY_ENABLED
#include <sentry.h>
#endif

namespace parties {

bool crash_reporter_is_crashpad_handler([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    // No-op: using inproc backend, no handler subprocess needed.
    return false;
}

void crash_reporter_init(const char* dsn, [[maybe_unused]] const char* exe_path) {
#ifdef SENTRY_ENABLED
    if (!dsn || dsn[0] == '\0') return;
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, dsn);
    sentry_options_set_release(options, APP_VERSION);
#ifdef PARTIES_RETAIL
    sentry_options_set_environment(options, "production");
#else
    sentry_options_set_environment(options, "development");
#endif
    sentry_options_set_database_path(options, ".sentry-native");
    sentry_options_set_auto_session_tracking(options, 1);
    sentry_init(options);
    sentry_start_session();
#else
    (void)dsn;
#endif
}

void crash_reporter_set_user(const std::string& user_id, const std::string& display_name) {
#ifdef SENTRY_ENABLED
    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "id", sentry_value_new_string(user_id.c_str()));
    sentry_value_set_by_key(user, "username", sentry_value_new_string(display_name.c_str()));
    sentry_set_user(user);
#else
    (void)user_id;
    (void)display_name;
#endif
}

void crash_reporter_shutdown() {
#ifdef SENTRY_ENABLED
    sentry_end_session();
    sentry_close();
#endif
}

} // namespace parties
