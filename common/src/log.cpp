#include <parties/log.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#elif defined(__APPLE__)
#include <os/log.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/null_mutex.h>
#include <mutex>
#endif

namespace parties {

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
        // Server: rotating log file (5 MB, 3 rotated files) + stdout
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "parties_server.log", 5 * 1024 * 1024, 3));
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
    logger->set_level(static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL));
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    spdlog::set_default_logger(std::move(logger));
}

void log_shutdown() {
    spdlog::shutdown();
}

} // namespace parties
