#include <parties/version.h>
#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>
#include <parties/log.h>
#include <parties/crash_reporter.h>
#include <server/config.h>
#include <server/server.h>

#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
#ifdef SENTRY_DSN_VALUE
    parties::crash_reporter_init(SENTRY_DSN_VALUE);
#else
    parties::crash_reporter_init(nullptr);
#endif
    parties::log_init(parties::LogTarget::Server);
    TracySetThreadName("Main");
    LOG_INFO("{} Server v{}", parties::APP_NAME, parties::APP_VERSION);

    if (!parties::crypto_init()) {
        LOG_ERROR("Failed to initialize crypto");
        return 1;
    }

    if (!parties::net_init()) {
        LOG_ERROR("Failed to initialize networking");
        parties::crypto_cleanup();
        return 1;
    }

    if (!parties::quic_init()) {
        LOG_ERROR("Failed to initialize QUIC");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    std::string config_path = "server.toml";
    if (argc > 1) config_path = argv[1];

    auto config = parties::server::Config::load(config_path);
    LOG_INFO("Server name: {}", config.server_name);
    LOG_INFO("QUIC port: {}", config.port);

    parties::server::Server server;
    if (!server.start(config)) {
        LOG_ERROR("Failed to start server");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("Server running. Press Ctrl+C to stop.");

    while (g_running) {
        server.run();
    }

    server.stop();
    parties::quic_cleanup();
    parties::net_cleanup();
    parties::crypto_cleanup();
    parties::log_shutdown();
    parties::crash_reporter_shutdown();
    return 0;
}
