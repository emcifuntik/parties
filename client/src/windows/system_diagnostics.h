#pragma once

namespace parties::client {

// Emit a compact, privacy-safe hardware/OS snapshot used to diagnose
// production-only performance and driver issues.
void log_windows_system_diagnostics();

} // namespace parties::client
