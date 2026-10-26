#pragma once

#include <RmlUi/Core/Core.h>

namespace parties::client {

class [[nodiscard]] RmlShutdownProof {
    friend RmlShutdownProof shutdown_rml();
    friend RmlShutdownProof rml_not_running();
    RmlShutdownProof() = default;
};

inline RmlShutdownProof shutdown_rml() {
    Rml::Shutdown();
    return {};
}

inline RmlShutdownProof rml_not_running() { return {}; }

}
