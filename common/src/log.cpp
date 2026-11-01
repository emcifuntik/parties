#include <parties/log.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <deque>
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

// In-memory tail of the client log for the diagnostics overlay.
constexpr size_t RECENT_LOG_LINES = 300;
std::mutex g_recent_mutex;
std::deque<RecentLogLine> g_recent;
uint64_t g_recent_seq = 0;

class recent_lines_sink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level < spdlog::level::info) return;
        const auto t = std::chrono::system_clock::to_time_t(msg.time);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            msg.time.time_since_epoch()).count() % 1000;
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[16];
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(ms));
        RecentLogLine line;
        line.level = static_cast<int>(msg.level);
        line.text = stamp;
        if (msg.source.filename) {
            const char* file = msg.source.filename;
            for (const char* p = file; *p; ++p)
                if (*p == '/' || *p == '\\') file = p + 1;
            line.text += ' ';
            line.text += file;
            line.text += ':';
            line.text += std::to_string(msg.source.line);
        }
        line.text += ' ';
        line.text.append(msg.payload.data(), msg.payload.size());
        std::lock_guard<std::mutex> lock(g_recent_mutex);
        line.seq = ++g_recent_seq;
        g_recent.push_back(std::move(line));
        while (g_recent.size() > RECENT_LOG_LINES) g_recent.pop_front();
    }
    void flush_() override {}
};

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
        sinks.push_back(std::make_shared<recent_lines_sink>());
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

std::vector<RecentLogLine> log_recent(uint64_t after_seq, size_t max_lines) {
    std::lock_guard<std::mutex> lock(g_recent_mutex);
    std::vector<RecentLogLine> out;
    auto it = std::find_if(g_recent.begin(), g_recent.end(),
                           [after_seq](const RecentLogLine& l) { return l.seq > after_seq; });
    const size_t available = static_cast<size_t>(std::distance(it, g_recent.end()));
    if (available > max_lines) it += static_cast<std::ptrdiff_t>(available - max_lines);
    out.assign(it, g_recent.end());
    return out;
}

void log_shutdown() {
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->flush();
    });
    spdlog::shutdown();
}

} // namespace parties
