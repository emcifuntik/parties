#include <parties/process_services.h>

#include <utility>

namespace parties {

ProcessServices::ProcessServices(const Options& options)
    : log_(options.log_target), crash_(log_, options.sentry_dsn) {
    auto net = NetLibrary::open();
    if (!net) {
        LOG_ERROR("Failed to initialize networking");
        return;
    }
    net_.emplace(std::move(*net));
    auto quic = QuicLibrary::open();
    if (!quic) {
        LOG_ERROR("Failed to initialize QUIC");
        return;
    }
    quic_.emplace(std::move(*quic));
}

std::unique_ptr<ProcessServices> ProcessServices::start(const Options& options) {
    std::unique_ptr<ProcessServices> services(new ProcessServices(options));
    if (!services->quic_) return nullptr;
    return services;
}

}
