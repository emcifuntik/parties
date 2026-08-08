#pragma once

#include <spdlog/spdlog.h>

#include <string>

// Logging macros — include source file:line via spdlog's SPDLOG_* macros.
// SPDLOG_ACTIVE_LEVEL (set in CMakeLists.txt) controls compile-time filtering.
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

namespace parties {

enum class LogTarget {
    Client,  // Rotating file + platform diagnostics; stdout is added for non-retail builds.
    Server,  // Rotating log file + stdout.
};

// Initialize logging. Call once at startup before any LOG_ macros.
void log_init(LogTarget target);

// Absolute path of the active rotating log file, or an empty string when the
// current platform could not create one. This is intended for support bundles
// and crash-report attachments.
std::string log_file_path();

// Flush and shut down logging. Call at shutdown.
void log_shutdown();

} // namespace parties
