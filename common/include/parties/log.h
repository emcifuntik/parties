#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <vector>

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

// The most recent client log lines (info and above), kept in memory for the
// diagnostics overlay. Empty for the server target.
struct RecentLogLine {
    uint64_t    seq = 0;        // increases by one per line
    int         level = 0;      // spdlog::level::level_enum
    std::string text;           // "HH:MM:SS.mmm file.cpp:123 message"
};
// Lines with seq > after_seq, at most max_lines, oldest first. Thread-safe.
std::vector<RecentLogLine> log_recent(uint64_t after_seq, size_t max_lines);

} // namespace parties
