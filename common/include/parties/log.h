#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>

#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

namespace parties {

enum class LogTarget {
    Client,
    Server,
};

class LogSession {
public:
    explicit LogSession(LogTarget target);
    ~LogSession();
    LogSession(const LogSession&) = delete;
    LogSession& operator=(const LogSession&) = delete;

    const std::optional<std::filesystem::path>& file_path() const { return file_path_; }

private:
    std::optional<std::filesystem::path> file_path_;
};

}
