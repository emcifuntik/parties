#pragma once

#include <string>

namespace parties {

// Check if launched as a crashpad handler subprocess.
// Must be called at the very start of main(), before any other code.
// Returns true if this process IS the handler (and has already exited via exit()).
// Returns false if normal app startup should continue.
bool crash_reporter_is_crashpad_handler(int argc, char* argv[]);

// Initialize Sentry crash reporting.
// Does nothing if Sentry is not linked or dsn is null/empty.
// Call once at startup, before any other initialization.
void crash_reporter_init(const char* dsn, const char* exe_path = nullptr);

// Set user identity after authentication succeeds.
void crash_reporter_set_user(const std::string& user_id, const std::string& display_name);

// Flush events and shut down. Call at shutdown.
void crash_reporter_shutdown();

} // namespace parties
