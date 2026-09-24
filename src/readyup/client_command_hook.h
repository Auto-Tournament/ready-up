#pragma once

namespace readyup {

// Installs an in-process hook that observes chat lines and routes `.ru ...`
// commands without relying on log files or cfg aliases.
void InstallClientCommandHook();

// True once the in-process logging listener is registered (it then sees every
// server log line, so the file-based log reader stands down).
bool InProcessLogListenerActive();

}  // namespace readyup
