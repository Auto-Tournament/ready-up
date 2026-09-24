#pragma once

#include <string>

namespace readyup {

// Hooks the server's ISource2Server::GameFrame (vtable slot) so we can run periodic logic on
// the server thread (safe for engine-facing operations).
//
// The slot is verified first (engine-surface vtable_indices "ISource2Server::GameFrame":
// CSource2Server vtable located via RTTI, slot target identity anchor, and the interface's vptr
// must be that vtable). Unverified => not patched, and the features that need a tick disable
// themselves. Safe to call repeatedly with any interface pointer; wrong objects are ignored.
void InstallGameFrameHook(void* source2ServerIface);

// Convenience helper that proactively resolves Source2Server001 and hooks it.
void InstallGameFrameHookAuto();

bool GameFrameHookInstalled();

// True once an install attempt was rejected (slot unverified); detail says why.
bool GameFrameHookFailed(std::string* detail);

// Number of simulating GameFrame ticks seen (0 until the first map is running).
unsigned long long GameFrameSimulatingTicks();

}  // namespace readyup
