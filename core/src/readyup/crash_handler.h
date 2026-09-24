#pragma once

namespace readyup {

// Installs a SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT handler that prints a backtrace to stderr and
// appends it to readyup_crash.log next to the shim, then restores the default action and re-raises
// the signal so the process really dies (tmux / the server manager can restart it) instead of
// hanging. Installed by default; READYUP_CRASH_HANDLER=0 opts out. Idempotent.
void InstallCrashHandlersMaybe();

}  // namespace readyup
