#include <parties/version.h>
#include <parties/process_services.h>
#include <parties/profiler.h>
#include <server/config.h>
#include <server/server.h>

#include <csignal>

static parties::server::Server* g_server = nullptr;

static void signal_handler(int) {
    g_server->request_stop();
}

#ifdef SENTRY_DSN_VALUE
static constexpr std::optional<std::string_view> kSentryDsn = SENTRY_DSN_VALUE;
#else
static constexpr std::optional<std::string_view> kSentryDsn = std::nullopt;
#endif

int main(int argc, char* argv[]) {
    const auto services = parties::ProcessServices::start({parties::LogTarget::Server, kSentryDsn});
    if (!services) return 1;
    TracySetThreadName("Main");
    LOG_INFO("{} Server v{}", parties::APP_NAME, parties::APP_VERSION);

    std::string config_path = "server.toml";
    if (argc > 1) config_path = argv[1];

    auto config = parties::server::Config::load(config_path);
    LOG_INFO("Server name: {}", config.server_name);
    LOG_INFO("QUIC port: {}", config.port);

    parties::server::Server server;
    if (!server.start(config)) {
        LOG_ERROR("Failed to start server");
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("Server running. Press Ctrl+C to stop.");

    server.run_until_stopped();
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    return 0;
}
