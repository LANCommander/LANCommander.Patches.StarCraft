#pragma once

namespace scpatch::crash {

// Logs fatal-looking exceptions (access violations etc.) with registers and a stack scan to scpatch.log.
void InstallLogger();

}  // namespace scpatch::crash
