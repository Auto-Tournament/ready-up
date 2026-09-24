#pragma once

namespace readyup {

// Installs a detour on CS2 server Host_Say, CounterStrikeSharp-style.
// Used for reliable interception of say/say_team and `.ru` parsing.
void InstallHostSayHook();

// Returns true if the Host_Say detour was successfully installed.
bool HostSayHookInstalled();

}  // namespace readyup

