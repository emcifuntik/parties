#include <client/server_query_wake.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using parties::client::ServerQueryWakeEvent;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "Server query wake test failed: %s\n", message);
    return condition;
}

} // namespace

int main() {
    bool success = true;

    // The production startup order publishes targets immediately after the
    // worker's first empty pass. Verify that this early signal is sticky.
    ServerQueryWakeEvent early;
    early.request();
    const auto early_start = std::chrono::steady_clock::now();
    success &= Check(early.wait_for_next(2s), "early request stopped the event");
    const auto early_elapsed = std::chrono::steady_clock::now() - early_start;
    success &= Check(early_elapsed < 100ms, "early request waited for the periodic timeout");

    // Also verify a request wakes a thread that is already waiting.
    ServerQueryWakeEvent waiting;
    std::atomic<bool> continued{false};
    const auto waiting_start = std::chrono::steady_clock::now();
    std::thread worker([&] { continued = waiting.wait_for_next(2s); });
    std::this_thread::sleep_for(20ms);
    waiting.request();
    worker.join();
    const auto waiting_elapsed = std::chrono::steady_clock::now() - waiting_start;
    success &= Check(continued.load(), "waiting request stopped the event");
    success &= Check(waiting_elapsed < 250ms, "waiting request did not wake promptly");

    ServerQueryWakeEvent stopped;
    stopped.stop();
    success &= Check(!stopped.wait_for_next(2s), "stop did not interrupt the wait");

    return success ? 0 : 1;
}
