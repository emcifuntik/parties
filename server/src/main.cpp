#include <parties/version.h>
#include <parties/crypto.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>
#include <parties/profiler.h>
#include <server/config.h>
#include <server/server.h>

#include <cstdio>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    TracySetThreadName("Main");
    std::printf("%s Server v%s\n", parties::APP_NAME, parties::APP_VERSION);

    if (!parties::crypto_init()) {
        std::fprintf(stderr, "Failed to initialize crypto\n");
        return 1;
    }

    if (!parties::net_init()) {
        std::fprintf(stderr, "Failed to initialize networking\n");
        parties::crypto_cleanup();
        return 1;
    }

    if (!parties::quic_init()) {
        std::fprintf(stderr, "Failed to initialize QUIC\n");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    // Load config
    std::string config_path = "server.toml";
    if (argc > 1) config_path = argv[1];

    auto config = parties::server::Config::load(config_path);
    std::printf("Server name: %s\n", config.server_name.c_str());
    std::printf("QUIC port: %u\n", config.port);

    // Start server
    parties::server::Server server;
    if (!server.start(config)) {
        std::fprintf(stderr, "Failed to start server\n");
        parties::net_cleanup();
        parties::crypto_cleanup();
        return 1;
    }

    // Handle Ctrl+C
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::printf("Server running. Press Ctrl+C to stop.\n");

    // Main loop — Server::run() processes messages in a non-blocking fashion
    while (g_running) {
        server.run();
    }

    server.stop();
    parties::quic_cleanup();
    parties::net_cleanup();
    parties::crypto_cleanup();
    return 0;
}
