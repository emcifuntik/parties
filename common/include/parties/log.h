#pragma once

#include <spdlog/spdlog.h>

#include <string>

#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

namespace parties {

enum class LogTarget {
    Client,
    Server,
};

void log_init(LogTarget target);

std::string log_file_path();

void log_shutdown();

}
