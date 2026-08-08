#include <parties/log.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#elif defined(__APPLE__)
#include <os/log.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/null_mutex.h>
#include <mutex>
#endif

namespace parties {

namespace {

std::mutex g_log_path_mutex;
std::string g_log_path;

std::string environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::filesystem::path client_log_directory() {
#ifdef _WIN32
    if (const std::string local_app_data = environment_value("LOCALAPPDATA");
        !local_app_data.empty()) {
        return std::filesystem::path(local_app_data) / "Parties" / "logs";
    }
#elif defined(__APPLE__)
    if (const std::string home = environment_value("HOME"); !home.empty())
        return std::filesystem::path(home) / "Library" / "Logs" / "Parties";
#else
    if (const std::string state_home = environment_value("XDG_STATE_HOME");
        !state_home.empty()) {
        return std::filesystem::path(state_home) / "parties";
    }
    if (const std::string home = environment_value("HOME"); !home.empty())
        return std::filesystem::path(home) / ".local" / "state" / "parties";
#endif
    return std::filesystem::current_path();
}

spdlog::level::level_enum configured_log_level() {
    std::string level = environment_value("PARTIES_LOG_LEVEL");
    if (level.empty()) return spdlog::level::info;
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (level == "debug") return spdlog::level::debug;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    return spdlog::level::info;
}

void remember_log_path(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    std::lock_guard lock(g_log_path_mutex);
    g_log_path = (ec ? path : absolute).string();
}

} // namespace

#ifdef __APPLE__
// os_log sink for macOS — maps spdlog levels to os_log levels
class oslog_sink : public spdlog::sinks::base_sink<std::mutex> {
public:
    oslog_sink() : log_(os_log_create("org.parties", "default")) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        auto formatted = std::string(msg.payload.data(), msg.payload.size());
        os_log_type_t type;
        switch (msg.level) {
        case spdlog::level::debug:
        case spdlog::level::trace:
            type = OS_LOG_TYPE_DEBUG;
            break;
        case spdlog::level::warn:
            type = OS_LOG_TYPE_DEFAULT;
            break;
        case spdlog::level::err:
        case spdlog::level::critical:
            type = OS_LOG_TYPE_ERROR;
            break;
        default:
            type = OS_LOG_TYPE_INFO;
            break;
        }
        os_log_with_type(log_, type, "%{public}s", formatted.c_str());
    }

    void flush_() override {}

private:
    os_log_t log_;
};
#endif

void log_init(LogTarget target) {
    std::vector<spdlog::sink_ptr> sinks;

    // Production client logs must survive after the console/debugger is gone.
    // Keep enough history for intermittent driver stalls without allowing logs
    // to grow without bounds.
    try {
        std::filesystem::path file_path;
        if (target == LogTarget::Client) {
            const auto directory = client_log_directory();
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!ec)
                file_path = directory / "parties_client.log";
        } else {
            file_path = "parties_server.log";
        }

        if (!file_path.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path.string(), 5 * 1024 * 1024, 3));
            remember_log_path(file_path);
        }
    } catch (const std::exception& error) {
#ifdef _WIN32
        const std::string message = std::string("Parties could not create its log file: ") +
            error.what() + "\n";
        OutputDebugStringA(message.c_str());
#endif
    }

    if (target == LogTarget::Client) {
#ifdef _WIN32
        // Always log to OutputDebugString (visible in debugger / DebugView)
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#ifndef PARTIES_RETAIL
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
#elif defined(__APPLE__)
        sinks.push_back(std::make_shared<oslog_sink>());
#ifndef PARTIES_RETAIL
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
#else
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
    } else {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
    const auto runtime_level = configured_log_level();
    logger->set_level(runtime_level);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [pid=%P tid=%t] [%s:%#] %v");
    logger->flush_on(spdlog::level::warn);
    logger->set_error_handler([](const std::string& message) {
#ifdef _WIN32
        const std::string output = "Parties logging error: " + message + "\n";
        OutputDebugStringA(output.c_str());
#else
        std::fprintf(stderr, "Parties logging error: %s\n", message.c_str());
#endif
    });
    spdlog::set_default_logger(std::move(logger));
    spdlog::flush_every(std::chrono::seconds(1));

    const auto path = log_file_path();
    if (!path.empty())
        LOG_INFO("Persistent log file: {}", path);
    else
        LOG_WARN("Persistent log file is unavailable; diagnostics will not survive process exit");
    const auto level_name = spdlog::level::to_string_view(runtime_level);
    LOG_INFO("Runtime log level: {}", std::string(level_name.data(), level_name.size()));
}

std::string log_file_path() {
    std::lock_guard lock(g_log_path_mutex);
    return g_log_path;
}

void log_shutdown() {
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->flush();
    });
    spdlog::shutdown();
}

} // namespace parties
