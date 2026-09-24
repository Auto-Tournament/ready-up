#pragma once

namespace readyup {

// Best-effort: attempt to register native console commands (e.g. `ru ...`).
// If registration is not possible on this server build, this is a no-op.
void TryRegisterConsoleCommands();

}  // namespace readyup

